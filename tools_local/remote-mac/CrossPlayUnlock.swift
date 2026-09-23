// CrossPlay Unlock -- the Mac half of the reader's unlock button.
//
// It answers challenges from the reader over a custom GATT service, and it is
// the only thing that decides whether the password leaves this machine. The
// reader holds no password: it holds a secret that proves it is the paired
// reader, and this agent sends the password -- encrypted under a key derived
// from that secret and a nonce that has never been used before -- only when
// its own screen is actually locked.
//
// Build and install: see README.md next to this file.
//
//   crossplay-unlock pair      store the reader's code and this Mac's password
//   crossplay-unlock password  store a new password, keeping the pairing
//   crossplay-unlock status    what it thinks it has
//   crossplay-unlock forget    delete both from the Keychain
//   crossplay-unlock run       serve challenges (what launchd runs)
//
// Every byte on the wire is defined by src/apps_local/remote/RemoteVault.h in
// the firmware, and host-tests/remotevault proves that file against RFC 4231,
// RFC 7914 and FIPS 180-4. If this and the reader ever disagree, one of them
// has drifted from that header.

import Foundation
import CoreBluetooth
import CryptoKit
import CoreGraphics
import Security

// MARK: - The wire, exactly as RemoteVault.h defines it

enum Wire {
    static let service = CBUUID(string: "6F1B0A00-9D3C-4F5E-8A77-2B4C1D6E9F01")
    static let challenge = CBUUID(string: "6F1B0A01-9D3C-4F5E-8A77-2B4C1D6E9F01")
    static let response = CBUUID(string: "6F1B0A02-9D3C-4F5E-8A77-2B4C1D6E9F01")

    static let version: UInt8 = 1
    static let nonceLen = 16
    static let macLen = 32
    static let challengeLen = 1 + 1 + 8 + nonceLen + macLen   // 58
    static let responseHeadLen = 1 + 1 + 8 + 1                // 11
    static let maxPayload = 96

    static let labelRequest = "crossplay/unlock/1/req"
    static let labelResponse = "crossplay/unlock/1/res"
    static let labelEncrypt = "crossplay/unlock/1/enc"

    enum Op: UInt8 { case status = 0x01, unlock = 0x02 }
    enum Screen: UInt8 { case unknown = 0x00, unlocked = 0x01, locked = 0x02 }
}

struct Challenge {
    var version: UInt8
    var op: Wire.Op
    var counter: UInt64
    var nonce: Data
    var mac: Data

    init?(_ frame: Data) {
        guard frame.count == Wire.challengeLen else { return nil }
        let b = [UInt8](frame)
        guard let op = Wire.Op(rawValue: b[1]) else { return nil }
        version = b[0]
        self.op = op
        var c: UInt64 = 0
        for i in 2..<10 { c = (c << 8) | UInt64(b[i]) }
        counter = c
        nonce = Data(b[10..<26])
        mac = Data(b[26..<58])
    }

    // version || op || counter || nonce -- what the reader signed.
    var signedBytes: Data {
        var out = Data([version, op.rawValue])
        out.append(Challenge.bigEndian(counter))
        out.append(nonce)
        return out
    }

    static func bigEndian(_ value: UInt64) -> Data {
        var out = Data(capacity: 8)
        for i in (0..<8).reversed() { out.append(UInt8((value >> (UInt64(i) * 8)) & 0xFF)) }
        return out
    }
}

enum Crypto {
    static func hmac(key: Data, data: Data) -> Data {
        Data(HMAC<SHA256>.authenticationCode(for: data, using: SymmetricKey(data: key)))
    }

    static func derive(secret: Data, label: String, nonce: Data) -> Data {
        var input = Data(label.utf8)
        input.append(nonce)
        return hmac(key: secret, data: input)
    }

    // HMAC-SHA256 in counter mode, the same stream the reader unseals with.
    static func keystream(key: Data, length: Int) -> Data {
        var out = Data()
        var index: UInt32 = 0
        while out.count < length {
            var counter = Data()
            for i in (0..<4).reversed() { counter.append(UInt8((index >> (UInt32(i) * 8)) & 0xFF)) }
            out.append(hmac(key: key, data: counter))
            index += 1
        }
        return out.prefix(length)
    }

    // Compares in time independent of where the first difference falls.
    static func equal(_ a: Data, _ b: Data) -> Bool {
        guard a.count == b.count else { return false }
        var difference: UInt8 = 0
        for (x, y) in zip(a, b) { difference |= x ^ y }
        return difference == 0
    }
}

// MARK: - The pairing code

// Crockford base32, uppercase, no padding, with the three folds the alphabet
// exists to allow: O for 0, I and L for 1.
enum Base32 {
    static let alphabet = Array("0123456789ABCDEFGHJKMNPQRSTVWXYZ")

    static func decode(_ text: String) -> Data? {
        var symbols: [UInt8] = []
        for raw in text.uppercased() {
            if raw == "-" || raw == " " { continue }
            let c: Character = (raw == "O") ? "0" : ((raw == "I" || raw == "L") ? "1" : raw)
            guard let index = alphabet.firstIndex(of: c) else { return nil }
            symbols.append(UInt8(index))
        }
        guard symbols.count == 32 else { return nil }
        var out = [UInt8](repeating: 0, count: 20)
        for i in 0..<32 {
            let bit = i * 5
            let byte = bit / 8
            let shift = bit % 8
            let window = UInt32(symbols[i]) << (11 - shift)
            out[byte] |= UInt8((window >> 8) & 0xFF)
            if byte + 1 < 20 { out[byte + 1] |= UInt8(window & 0xFF) }
        }
        return Data(out)
    }
}

// MARK: - The Keychain

// kSecAttrAccessibleAfterFirstUnlock, deliberately: this agent has to read
// both items while the SCREEN is locked, which is the only time it matters.
// Screen lock is not Keychain lock -- the login keychain stays unlocked for
// the duration of the session -- so this works and does not weaken anything.
enum Store {
    static let service = "com.crossplay.unlock"

    static func set(_ account: String, _ value: Data) {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service,
                                    kSecAttrAccount as String: account]
        SecItemDelete(query as CFDictionary)
        var add = query
        add[kSecValueData as String] = value
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        let status = SecItemAdd(add as CFDictionary, nil)
        if status != errSecSuccess { FileHandle.standardError.write("keychain write failed: \(status)\n".data(using: .utf8)!) }
    }

    static func get(_ account: String) -> Data? {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service,
                                    kSecAttrAccount as String: account,
                                    kSecReturnData as String: true,
                                    kSecMatchLimit as String: kSecMatchLimitOne]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess else { return nil }
        return item as? Data
    }

    static func remove(_ account: String) {
        SecItemDelete([kSecClass as String: kSecClassGenericPassword,
                       kSecAttrService as String: service,
                       kSecAttrAccount as String: account] as CFDictionary)
    }
}

// MARK: - Replay and rate limiting

// The reader's counter only ever goes up, so anything at or below what we have
// already accepted is a recording being played back at us.
final class Ledger {
    private let path: URL
    private(set) var highWater: UInt64 = 0
    private(set) var strikes: Int = 0
    private var blockedUntil: Date = .distantPast

    init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CrossPlayUnlock", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        path = base.appendingPathComponent("ledger.json")
        if let data = try? Data(contentsOf: path),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            highWater = (json["counter"] as? NSNumber)?.uint64Value ?? 0
        }
    }

    func accept(_ counter: UInt64) -> Bool {
        guard counter > highWater else { return false }
        highWater = counter
        let json: [String: Any] = ["counter": NSNumber(value: highWater)]
        try? JSONSerialization.data(withJSONObject: json).write(to: path)
        return true
    }

    // A wrong MAC is a wrong PIN or a stranger, and the reader cannot tell
    // which. This is the only place either can be counted, which is what makes
    // a four-digit PIN worth having: there is no offline oracle, so every guess
    // has to come through here.
    var isBlocked: Bool { Date() < blockedUntil }

    func strike() {
        strikes += 1
        if strikes >= 5 {
            // Doubling, from half a minute. Five wrong answers is already well
            // past a mistyped PIN.
            let seconds = min(pow(2.0, Double(strikes - 5)) * 30.0, 3600.0)
            blockedUntil = Date().addingTimeInterval(seconds)
            log("blocking for \(Int(seconds))s after \(strikes) bad answers")
        }
    }

    func clearStrikes() { strikes = 0; blockedUntil = .distantPast }
}

func log(_ message: String) {
    let stamp = ISO8601DateFormatter().string(from: Date())
    print("[\(stamp)] \(message)")
    fflush(stdout)
}

// MARK: - Is the screen actually locked?

func screenIsLocked() -> Bool {
    guard let session = CGSessionCopyCurrentDictionary() as? [String: Any] else { return false }
    return (session["CGSSessionScreenIsLocked"] as? Bool) ?? false
}

// MARK: - The agent

final class Agent: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var reader: CBPeripheral?
    private var responseCharacteristic: CBCharacteristic?
    private let ledger = Ledger()
    private let secret: Data

    init(secret: Data) {
        self.secret = secret
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func centralManagerDidUpdateState(_ manager: CBCentralManager) {
        guard manager.state == .poweredOn else {
            log("bluetooth is \(manager.state.rawValue); waiting")
            return
        }
        // macOS has almost certainly connected the reader already, for HID.
        // A CoreBluetooth central may talk GATT to a system-connected
        // peripheral, so the usual path finds it here and never scans.
        let connected = manager.retrieveConnectedPeripherals(withServices: [Wire.service])
        if let found = connected.first {
            log("found the reader among the peripherals macOS already has")
            attach(found)
        } else {
            log("scanning")
            manager.scanForPeripherals(withServices: [Wire.service])
        }
    }

    private func attach(_ peripheral: CBPeripheral) {
        reader = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ manager: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        manager.stopScan()
        log("found the reader by scanning")
        attach(peripheral)
    }

    func centralManager(_ manager: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("connected")
        peripheral.discoverServices([Wire.service])
    }

    func centralManager(_ manager: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        log("disconnected; waiting for it to come back")
        responseCharacteristic = nil
        // The reader takes its radio down when the app closes, so a
        // disconnection is normal rather than a failure. Reconnect stays
        // pending until it advertises again.
        manager.connect(peripheral)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Wire.service }) else {
            log("the reader has no unlock service")
            return
        }
        peripheral.discoverCharacteristics([Wire.challenge, Wire.response], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        for characteristic in service.characteristics ?? [] {
            if characteristic.uuid == Wire.challenge {
                peripheral.setNotifyValue(true, for: characteristic)
                log("listening for challenges")
            }
            if characteristic.uuid == Wire.response { responseCharacteristic = characteristic }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == Wire.challenge, let frame = characteristic.value else { return }
        handle(frame)
    }

    private func handle(_ frame: Data) {
        guard let challenge = Challenge(frame) else {
            log("a frame arrived that is not a challenge")
            return
        }
        guard challenge.version == Wire.version else {
            log("challenge speaks version \(challenge.version); we speak \(Wire.version)")
            return
        }
        if ledger.isBlocked {
            log("blocked; ignoring")
            return
        }

        let requestKey = Crypto.derive(secret: secret, label: Wire.labelRequest, nonce: challenge.nonce)
        let expected = Crypto.hmac(key: requestKey, data: challenge.signedBytes)
        guard Crypto.equal(expected, challenge.mac) else {
            ledger.strike()
            log("a challenge arrived that this reader did not sign")
            return
        }
        guard ledger.accept(challenge.counter) else {
            log("challenge \(challenge.counter) is at or below \(ledger.highWater); replayed")
            return
        }
        ledger.clearStrikes()

        let locked = screenIsLocked()
        let screen: Wire.Screen = locked ? .locked : .unlocked

        // The password goes out ONLY for an unlock request at a screen that is
        // actually locked. A status request is answered with the state alone,
        // and so is an unlock request at a Mac that is already awake -- which
        // is what stops the reader typing into whatever has focus.
        var payload = Data()
        if challenge.op == .unlock && locked {
            if let password = Store.get("password"), password.count <= Wire.maxPayload {
                let encryptKey = Crypto.derive(secret: secret, label: Wire.labelEncrypt, nonce: challenge.nonce)
                let stream = Crypto.keystream(key: encryptKey, length: password.count)
                payload = Data(zip(password, stream).map { $0 ^ $1 })
                log("unlock: locked, sending")
            } else {
                log("unlock: locked, but no password is stored (or it is too long)")
            }
        } else {
            log("\(challenge.op == .unlock ? "unlock" : "status"): screen is \(locked ? "locked" : "awake")")
        }

        var body = Data([Wire.version, screen.rawValue])
        body.append(Challenge.bigEndian(challenge.counter))
        body.append(UInt8(payload.count))
        body.append(payload)

        // Encrypt-then-MAC, over the challenge's own version and op as well as
        // the answer: a status answer cannot stand in for an unlock one, and
        // the screen state cannot be flipped in flight.
        var macInput = challenge.signedBytes
        macInput.append(screen.rawValue)
        macInput.append(UInt8(payload.count))
        macInput.append(payload)
        let responseKey = Crypto.derive(secret: secret, label: Wire.labelResponse, nonce: challenge.nonce)
        body.append(Crypto.hmac(key: responseKey, data: macInput))

        guard let characteristic = responseCharacteristic, let peripheral = reader else {
            log("nowhere to write the answer")
            return
        }
        peripheral.writeValue(body, for: characteristic, type: .withResponse)
    }
}

// MARK: - Commands

func readLine(prompt: String, secret: Bool = false) -> String {
    FileHandle.standardError.write(prompt.data(using: .utf8)!)
    if secret, let raw = getpass("") { return String(cString: raw) }
    return Swift.readLine() ?? ""
}

func commandPair() {
    let code = readLine(prompt: "Pairing code from the reader (8 groups of 4): ")
    guard let secret = Base32.decode(code) else {
        print("That is not a pairing code: 32 symbols, letters and digits.")
        exit(1)
    }
    let password = readLine(prompt: "This Mac's login password: ", secret: true)
    guard !password.isEmpty else {
        print("No password, nothing to unlock with.")
        exit(1)
    }
    guard password.utf8.count <= Wire.maxPayload else {
        print("That password is longer than \(Wire.maxPayload) bytes, which is more than the reader will type.")
        exit(1)
    }
    Store.set("secret", secret)
    Store.set("password", Data(password.utf8))
    print("Paired. Both are in the login Keychain under \(Store.service).")
}

// Changing the Mac's login password does not touch the pairing, and re-pairing
// to fix it would mean a new code and a new PIN for something neither of them
// is wrong about. So this updates the one thing that went stale.
func commandPassword() {
    guard Store.get("secret") != nil else {
        print("Not paired, so there is nothing to keep. Run: crossplay-unlock pair")
        exit(1)
    }
    let password = readLine(prompt: "This Mac's login password: ", secret: true)
    guard !password.isEmpty else {
        print("No password, nothing to unlock with.")
        exit(1)
    }
    guard password.utf8.count <= Wire.maxPayload else {
        print("That password is longer than \(Wire.maxPayload) bytes, which is more than the reader will type.")
        exit(1)
    }
    Store.set("password", Data(password.utf8))
    print("Stored. The pairing and its counter are untouched, so the reader needs no change.")
}

func commandStatus() {
    print("secret:   \(Store.get("secret") != nil ? "stored" : "missing")")
    print("password: \(Store.get("password") != nil ? "stored" : "missing")")
    print("screen:   \(screenIsLocked() ? "locked" : "awake")")
    print("counter:  \(Ledger().highWater)")
}

func commandForget() {
    Store.remove("secret")
    Store.remove("password")
    print("Forgotten. Unpair the reader in its own app too.")
}

// File scope on purpose: CBCentralManager holds its delegate weakly, so an
// agent kept only in a local would be deallocated the moment commandRun
// returned into dispatchMain and nothing would ever answer a challenge.
var runningAgent: Agent?

func commandRun() {
    guard let secret = Store.get("secret") else {
        print("Not paired. Run: crossplay-unlock pair")
        exit(1)
    }
    runningAgent = Agent(secret: secret)
    log("crossplay-unlock running")
    dispatchMain()
}

switch CommandLine.arguments.dropFirst().first ?? "run" {
case "pair": commandPair()
case "password": commandPassword()
case "status": commandStatus()
case "forget": commandForget()
case "run": commandRun()
default:
    print("usage: crossplay-unlock [pair|password|status|forget|run]")
    exit(2)
}
