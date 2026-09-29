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
//   crossplay-unlock unblock   lift the lockout after ten unverifiable requests
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
import CoreAudio

// MARK: - The wire, exactly as RemoteVault.h defines it

enum Wire {
    static let service = CBUUID(string: "6F1B0A00-9D3C-4F5E-8A77-2B4C1D6E9F01")
    static let challenge = CBUUID(string: "6F1B0A01-9D3C-4F5E-8A77-2B4C1D6E9F01")
    static let response = CBUUID(string: "6F1B0A02-9D3C-4F5E-8A77-2B4C1D6E9F01")
    static let nowPlaying = CBUUID(string: "6F1B0A03-9D3C-4F5E-8A77-2B4C1D6E9F01")
    static let command = CBUUID(string: "6F1B0A04-9D3C-4F5E-8A77-2B4C1D6E9F01")
    static let macState = CBUUID(string: "6F1B0A05-9D3C-4F5E-8A77-2B4C1D6E9F01")

    // The microphone button. [version, command] from the reader, [version,
    // flags] back; host-tests/remote pins both.
    static let macLinkVersion: UInt8 = 1
    static let commandMute: UInt8 = 0x01
    static let commandUnmute: UInt8 = 0x02
    static let flagMicrophonesMuted: UInt8 = 0x01

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
    // Ten unverifiable requests in a row, and the helper stops answering
    // altogether until someone at the Mac runs `crossplay-unlock unblock`. The
    // paired reader never sends one, so a run of them is something else
    // trying: it gets ten tries, then a person has to intervene.
    static let lockoutAfter = 10

    private let path: URL
    private(set) var highWater: UInt64 = 0
    private(set) var strikes: Int = 0
    private(set) var lockedOut = false
    private var blockedUntil: Date = .distantPast

    init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CrossPlayUnlock", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        path = base.appendingPathComponent("ledger.json")
        reload()
    }

    // The file is the truth, not this object. `pair` and `unblock` are
    // separate processes that rewrite it while the agent runs, and an agent
    // holding a copy from startup would answer a fresh pairing with
    // "replayed" and stay locked out after an unblock -- so it re-reads before
    // every request rather than trusting what it read at launch.
    func reload() {
        highWater = 0
        strikes = 0
        lockedOut = false
        if let data = try? Data(contentsOf: path),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            highWater = (json["counter"] as? NSNumber)?.uint64Value ?? 0
            // Persisted, so a restart -- a crash, a reboot, launchd's KeepAlive
            // -- does not hand a guesser a fresh ten.
            strikes = (json["strikes"] as? NSNumber)?.intValue ?? 0
            lockedOut = (json["lockedOut"] as? NSNumber)?.boolValue ?? false
        }
    }

    func accept(_ counter: UInt64) -> Bool {
        guard counter > highWater else { return false }
        highWater = counter
        save()
        return true
    }

    // A NEW pairing starts a new counter on the reader, so the old high-water
    // mark belongs to a conversation that no longer exists. Left in place it
    // refuses every challenge the new pairing sends until the reader climbs
    // back past it -- and since the reader has no way to be told, that reads
    // as an unlock button that simply stopped working.
    func reset() {
        highWater = 0
        strikes = 0
        lockedOut = false
        blockedUntil = .distantPast
        save()
    }

    // Lifts a lockout and clears the count, keeping the counter: the pairing
    // is still the same conversation.
    func unblock() {
        strikes = 0
        lockedOut = false
        blockedUntil = .distantPast
        save()
    }

    private func save() {
        let json: [String: Any] = ["counter": NSNumber(value: highWater),
                                   "strikes": NSNumber(value: strikes),
                                   "lockedOut": NSNumber(value: lockedOut)]
        try? JSONSerialization.data(withJSONObject: json).write(to: path)
    }

    // A wrong MAC is a stranger, or a reader paired to a different secret.
    // This is the only place either can be counted.
    var isBlocked: Bool { lockedOut || Date() < blockedUntil }

    func strike() {
        strikes += 1
        if strikes >= Ledger.lockoutAfter {
            lockedOut = true
            log("LOCKED OUT after \(strikes) wrong answers in a row. Run: crossplay-unlock unblock")
        } else if strikes >= 5 {
            // Slowing down from the fifth, doubling from half a minute, so the
            // ten cannot be spent in the few seconds they would otherwise take.
            let seconds = min(pow(2.0, Double(strikes - 5)) * 30.0, 3600.0)
            blockedUntil = Date().addingTimeInterval(seconds)
            log("pausing \(Int(seconds))s after \(strikes) wrong answers")
        }
        save()
    }

    func clearStrikes() {
        guard strikes != 0 else { return }
        strikes = 0
        blockedUntil = .distantPast
        save()
    }
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

// MARK: - Now playing

// What Music and Spotify say about themselves. Both broadcast a distributed
// notification on every play, pause, stop and track change, with the song in
// its userInfo -- so this needs no polling and, unlike AppleScript, no
// Automation permission prompt.
//
// What it cannot see: anything else. A browser playing YouTube does not
// broadcast, and the system-wide now-playing API is closed to third-party
// processes on current macOS.
enum NowPlaying {
    static let version: UInt8 = 1
    static let fieldMax = 64

    enum State: UInt8 { case nothing = 0, playing = 1, paused = 2 }

    struct Heard {
        var state: State
        var title: String
        var artist: String
        var at: Date
    }

    static let sources: [(player: String, notification: String)] = [
        ("Music", "com.apple.Music.playerInfo"),
        ("Spotify", "com.spotify.client.PlaybackStateChanged"),
    ]

    static func heard(from info: [AnyHashable: Any]?) -> Heard {
        let playerState = info?["Player State"] as? String ?? ""
        let state: State = playerState == "Playing" ? .playing : (playerState == "Paused" ? .paused : .nothing)
        return Heard(state: state,
                     title: info?["Name"] as? String ?? "",
                     artist: info?["Artist"] as? String ?? "",
                     at: Date())
    }

    // The most recent player that is PLAYING wins; failing that, the most
    // recent that is paused; failing that, nothing. So pausing Spotify while
    // Music plays leaves Music on the reader, which is what is audible.
    static func choose(_ heard: [String: Heard]) -> Heard? {
        let live = heard.values.filter { $0.state == .playing && !$0.title.isEmpty }
        if let newest = live.max(by: { $0.at < $1.at }) { return newest }
        let held = heard.values.filter { $0.state == .paused && !$0.title.isEmpty }
        return held.max(by: { $0.at < $1.at })
    }

    // Cut at a CHARACTER, never through one: the reader drops a half-character
    // anyway, but a title that loses its last letter to a byte limit is worse
    // than one that loses a whole word to an ellipsis on the reader's side.
    static func clip(_ text: String) -> Data {
        var out = Data()
        for ch in text {
            let bytes = Data(String(ch).utf8)
            if out.count + bytes.count > fieldMax { break }
            out.append(bytes)
        }
        return out
    }

    // The layout RemoteCore.h documents and host-tests/remote pins byte for
    // byte: version, state, title length, title, artist length, artist.
    static func frame(_ chosen: Heard?) -> Data {
        guard let chosen = chosen else { return Data([version, State.nothing.rawValue, 0, 0]) }
        let title = clip(chosen.title)
        let artist = clip(chosen.artist)
        var out = Data([version, chosen.state.rawValue, UInt8(title.count)])
        out.append(title)
        out.append(UInt8(artist.count))
        out.append(artist)
        return out
    }
}

// MARK: - The microphones

// Mutes every input device the Mac has: the built-in one if there is one --
// a Mac mini has none -- plus AirPods, USB and display microphones, and any
// plugged in while the mute is on.
//
// Through CoreAudio, device by device. A device with a mute control is muted
// with it; one without gets its input volume set to zero, and the level it
// had is remembered so unmuting puts it back.
//
// What it does NOT do: the camera. macOS has no supported way for a program
// to switch a camera off. The only system-level switch is a device-management
// profile of the kind an employer's IT installs.
final class Microphones {
    // True between a mute from the reader and the next unmute. While it is on,
    // anything that turns an input back up -- an app's automatic gain, a new
    // headset -- is turned back down.
    private(set) var wanted = false
    private var savedVolumes: [AudioObjectID: [AudioObjectPropertyElement: Float32]] = [:]
    private var timer: Timer?
    var onChange: (() -> Void)?

    init() {
        // A mute from before a restart is still a mute: CoreAudio keeps device
        // state, so if everything is silent now, keep it that way.
        let inputs = Microphones.inputDevices()
        wanted = !inputs.isEmpty && inputs.allSatisfy { isMuted($0) }
        var devices = Microphones.address(kAudioHardwarePropertyDevices)
        _ = AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &devices, DispatchQueue.main) {
            [weak self] _, _ in
            self?.enforce()
            self?.onChange?()
        }
        timer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            self?.enforce()
        }
    }

    static func address(_ selector: AudioObjectPropertySelector,
                        _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                        _ element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain)
        -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    }

    static func inputDevices() -> [AudioObjectID] {
        var addr = address(kAudioHardwarePropertyDevices)
        let system = AudioObjectID(kAudioObjectSystemObject)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(system, &addr, 0, nil, &size) == noErr, size > 0 else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(system, &addr, 0, nil, &size, &ids) == noErr else { return [] }
        return ids.filter { id in
            var streams = address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput)
            var streamSize: UInt32 = 0
            return AudioObjectGetPropertyDataSize(id, &streams, 0, nil, &streamSize) == noErr && streamSize > 0
        }
    }

    static func name(_ id: AudioObjectID) -> String {
        var addr = address(kAudioObjectPropertyName)
        var name: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &name) == noErr, let value = name else {
            return "input \(id)"
        }
        return value.takeRetainedValue() as String
    }

    private static func settable(_ id: AudioObjectID, _ addr: inout AudioObjectPropertyAddress) -> Bool {
        guard AudioObjectHasProperty(id, &addr) else { return false }
        var ok: DarwinBoolean = false
        return AudioObjectIsPropertySettable(id, &addr, &ok) == noErr && ok.boolValue
    }

    // The main volume control if the device has one, else its per-channel ones.
    private func volumeElements(_ id: AudioObjectID) -> [AudioObjectPropertyElement] {
        var found: [AudioObjectPropertyElement] = []
        for element: AudioObjectPropertyElement in [kAudioObjectPropertyElementMain, 1, 2] {
            var addr = Microphones.address(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeInput, element)
            if Microphones.settable(id, &addr) { found.append(element) }
        }
        return found.contains(kAudioObjectPropertyElementMain) ? [kAudioObjectPropertyElementMain] : found
    }

    private func volume(_ id: AudioObjectID, _ element: AudioObjectPropertyElement) -> Float32? {
        var addr = Microphones.address(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeInput, element)
        var value: Float32 = 0
        var size = UInt32(MemoryLayout<Float32>.size)
        return AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &value) == noErr ? value : nil
    }

    private func setVolume(_ id: AudioObjectID, _ element: AudioObjectPropertyElement, _ level: Float32) {
        var addr = Microphones.address(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeInput, element)
        var value = level
        _ = AudioObjectSetPropertyData(id, &addr, 0, nil, UInt32(MemoryLayout<Float32>.size), &value)
    }

    func isMuted(_ id: AudioObjectID) -> Bool {
        var mute = Microphones.address(kAudioDevicePropertyMute, kAudioObjectPropertyScopeInput)
        if AudioObjectHasProperty(id, &mute) {
            var value: UInt32 = 0
            var size = UInt32(MemoryLayout<UInt32>.size)
            if AudioObjectGetPropertyData(id, &mute, 0, nil, &size, &value) == noErr, value != 0 { return true }
        }
        let elements = volumeElements(id)
        return !elements.isEmpty && elements.allSatisfy { (volume(id, $0) ?? 1) <= 0.0001 }
    }

    // False when the device offers neither a mute nor a volume it lets anyone
    // set, which is the one case this cannot silence. It is logged by name.
    @discardableResult
    private func setMuted(_ id: AudioObjectID, _ muted: Bool) -> Bool {
        var mute = Microphones.address(kAudioDevicePropertyMute, kAudioObjectPropertyScopeInput)
        if Microphones.settable(id, &mute) {
            var value: UInt32 = muted ? 1 : 0
            if AudioObjectSetPropertyData(id, &mute, 0, nil, UInt32(MemoryLayout<UInt32>.size), &value) == noErr {
                return true
            }
        }
        let elements = volumeElements(id)
        guard !elements.isEmpty else { return false }
        for element in elements {
            if muted {
                if let level = volume(id, element), level > 0.0001 { savedVolumes[id, default: [:]][element] = level }
                setVolume(id, element, 0)
            } else {
                setVolume(id, element, savedVolumes[id]?[element] ?? 0.75)
            }
        }
        if !muted { savedVolumes[id] = nil }
        return true
    }

    var allMuted: Bool {
        let inputs = Microphones.inputDevices()
        return inputs.allSatisfy { isMuted($0) }
    }

    func mute() {
        wanted = true
        for id in Microphones.inputDevices() where !setMuted(id, true) {
            log("microphones: cannot mute \(Microphones.name(id)); it offers no mute and no volume")
        }
        log("microphones: muted \(Microphones.inputDevices().filter { isMuted($0) }.count) of \(Microphones.inputDevices().count)")
    }

    func unmute() {
        wanted = false
        for id in Microphones.inputDevices() { setMuted(id, false) }
        log("microphones: unmuted")
    }

    // Puts back anything that came back on while the mute was wanted.
    func enforce() {
        guard wanted else { return }
        var changed = false
        for id in Microphones.inputDevices() where !isMuted(id) {
            if setMuted(id, true) {
                changed = true
                log("microphones: \(Microphones.name(id)) came back on; muted it again")
            }
        }
        if changed { onChange?() }
    }
}

// MARK: - The agent

final class Agent: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var reader: CBPeripheral?
    private var responseCharacteristic: CBCharacteristic?
    private var nowPlayingCharacteristic: CBCharacteristic?
    private var macStateCharacteristic: CBCharacteristic?
    private let microphones = Microphones()
    private var lastMacState: Data?
    private let ledger = Ledger()
    private let secret: Data

    private var heard: [String: NowPlaying.Heard] = [:]
    private var observers: [NSObjectProtocol] = []
    // What the reader was last sent. nil means "send whatever is current the
    // next time there is somewhere to send it", which is the state after every
    // reconnect: the reader forgets the song when its radio goes down.
    private var lastSent: Data?

    init(secret: Data) {
        self.secret = secret
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
        microphones.onChange = { [weak self] in self?.sendMacState() }
        let centre = DistributedNotificationCenter.default()
        for source in NowPlaying.sources {
            observers.append(centre.addObserver(forName: NSNotification.Name(source.notification), object: nil,
                                                queue: .main) { [weak self] note in
                self?.playerChanged(source.player, note.userInfo)
            })
        }
    }

    private func playerChanged(_ player: String, _ info: [AnyHashable: Any]?) {
        heard[player] = NowPlaying.heard(from: info)
        sendNowPlaying()
    }

    private func sendNowPlaying() {
        let frame = NowPlaying.frame(NowPlaying.choose(heard))
        guard frame != lastSent else { return }
        guard let characteristic = nowPlayingCharacteristic, let peripheral = reader else { return }
        // With a response, so a failure comes back to didWriteValueFor and into
        // the log rather than vanishing.
        peripheral.writeValue(frame, for: characteristic, type: .withResponse)
        lastSent = frame
        if let chosen = NowPlaying.choose(heard) {
            log("now playing: \(chosen.title) by \(chosen.artist) (\(chosen.state == .playing ? "playing" : "paused"))")
        } else {
            log("now playing: nothing")
        }
    }

    private func sendMacState() {
        let flags: UInt8 = microphones.allMuted ? Wire.flagMicrophonesMuted : 0
        let frame = Data([Wire.macLinkVersion, flags])
        guard frame != lastMacState else { return }
        guard let characteristic = macStateCharacteristic, let peripheral = reader else { return }
        peripheral.writeValue(frame, for: characteristic, type: .withResponse)
        lastMacState = frame
    }

    private func handleCommand(_ frame: Data) {
        let bytes = [UInt8](frame)
        guard bytes.count == 2, bytes[0] == Wire.macLinkVersion else {
            log("a command arrived that is not one")
            return
        }
        switch bytes[1] {
        case Wire.commandMute: microphones.mute()
        case Wire.commandUnmute: microphones.unmute()
        default:
            log("an unknown command \(bytes[1]) was ignored")
            return
        }
        // Reported from what the devices now say, not from what was asked:
        // the reader's button fills only if the mute really took.
        lastMacState = nil
        sendMacState()
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let error = error else { return }
        log("write to \(characteristic.uuid) failed: \(error.localizedDescription)")
        // Try again next time rather than believing the reader has it.
        if characteristic.uuid == Wire.nowPlaying { lastSent = nil }
        if characteristic.uuid == Wire.macState { lastMacState = nil }
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
        nowPlayingCharacteristic = nil
        lastSent = nil
        macStateCharacteristic = nil
        lastMacState = nil
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
        peripheral.discoverCharacteristics([Wire.challenge, Wire.response, Wire.nowPlaying, Wire.command,
                                            Wire.macState], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        for characteristic in service.characteristics ?? [] {
            if characteristic.uuid == Wire.challenge {
                peripheral.setNotifyValue(true, for: characteristic)
                log("listening for challenges")
            }
            if characteristic.uuid == Wire.response { responseCharacteristic = characteristic }
            if characteristic.uuid == Wire.command { peripheral.setNotifyValue(true, for: characteristic) }
            if characteristic.uuid == Wire.macState {
                macStateCharacteristic = characteristic
                lastMacState = nil
                sendMacState()
            }
            if characteristic.uuid == Wire.nowPlaying {
                nowPlayingCharacteristic = characteristic
                // The reader has just (re)connected and knows nothing; tell it.
                lastSent = nil
                sendNowPlaying()
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard let frame = characteristic.value else { return }
        if characteristic.uuid == Wire.command {
            handleCommand(frame)
            return
        }
        guard characteristic.uuid == Wire.challenge else { return }
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
        ledger.reload()
        if ledger.isBlocked {
            log(ledger.lockedOut ? "locked out; ignoring. Run: crossplay-unlock unblock" : "pausing; ignoring")
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
    // Before anything can arrive under the new secret.
    Ledger().reset()
    print("Paired. Both are in the login Keychain under \(Store.service), and the replay counter is back to zero.")
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
    let ledger = Ledger()
    print("counter:  \(ledger.highWater)")
    print("wrong:    \(ledger.strikes) in a row\(ledger.lockedOut ? " -- LOCKED OUT, run: crossplay-unlock unblock" : "")")
    let inputs = Microphones.inputDevices()
    let mics = Microphones()
    print("mics:     \(inputs.filter { mics.isMuted($0) }.count) of \(inputs.count) muted")
}

func commandUnblock() {
    Ledger().unblock()
    print("Unblocked. The reader can ask again; the pairing is unchanged.")
}

func commandForget() {
    Store.remove("secret")
    Store.remove("password")
    Ledger().reset()
    print("Forgotten. Unpair the reader in its own app too.")
}

// File scope on purpose: CBCentralManager holds its delegate weakly, so an
// agent kept only in a local would be deallocated the moment commandRun
// returned into the run loop and nothing would ever answer a challenge.
var runningAgent: Agent?

func commandRun() {
    guard let secret = Store.get("secret") else {
        print("Not paired. Run: crossplay-unlock pair")
        exit(1)
    }
    runningAgent = Agent(secret: secret)
    log("crossplay-unlock running")
    // RunLoop, not dispatchMain(): distributed notifications arrive through a
    // run-loop source, which dispatchMain() never services. The run loop drains
    // the main queue too, so CoreBluetooth's callbacks still arrive.
    RunLoop.main.run()
}

switch CommandLine.arguments.dropFirst().first ?? "run" {
case "pair": commandPair()
case "password": commandPassword()
case "unblock": commandUnblock()
case "status": commandStatus()
case "forget": commandForget()
case "run": commandRun()
default:
    print("usage: crossplay-unlock [pair|password|unblock|status|forget|run]")
    exit(2)
}
