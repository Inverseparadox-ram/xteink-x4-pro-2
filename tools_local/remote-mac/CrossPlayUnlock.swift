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
    static let status = CBUUID(string: "6F1B0A06-9D3C-4F5E-8A77-2B4C1D6E9F01")

    // Pages 2 and 3 of the remote. One write per row; RemoteCore.h's "Status
    // boards" is the other half and host-tests/remote pins the bytes.
    static let statusVersion: UInt8 = 1
    static let boardClaude: UInt8 = 1
    static let boardServices: UInt8 = 2
    static let statusRowsMax = 10
    static let statusTitleMax = 48
    static let statusDetailMax = 32

    // The microphone button. [version, command] from the reader, [version,
    // flags] back; host-tests/remote pins both.
    static let macLinkVersion: UInt8 = 1
    static let commandMute: UInt8 = 0x01
    static let commandUnmute: UInt8 = 0x02
    // [version, 0x03, row, check]: restart the service on MAC-page row `row`,
    // whose title's FNV-1a low byte is `check` (RemoteCore's serviceCheck).
    static let commandRestart: UInt8 = 0x03
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

    // The canonical spelling of a secret, in the reader's eight groups of
    // four, so `pair` can show back what it understood.
    static func encode(_ secret: Data) -> String {
        let bytes = [UInt8](secret)
        guard bytes.count == 20 else { return "" }
        var out = ""
        for i in 0..<32 {
            let bit = i * 5
            let byte = bit / 8
            let shift = bit % 8
            var window = UInt32(bytes[byte]) << 8
            if byte + 1 < 20 { window |= UInt32(bytes[byte + 1]) }
            if i > 0 && i % 4 == 0 { out += i % 8 == 0 ? "  " : " " }
            out.append(alphabet[Int((window >> UInt32(11 - shift)) & 0x1F)])
        }
        return out
    }

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


// MARK: - Pages 2 and 3: status boards

enum StatusCode: UInt8 {
    case unknown = 0, inProcess = 1, awaitingInput = 2, completed = 3, failed = 4, running = 5, stopped = 6
}

struct StatusRow: Equatable {
    var status: StatusCode
    var title: String
    var detail: String
}

// UTF-8 cut at a character boundary, never through one.
func utf8Prefix(_ text: String, _ maxBytes: Int) -> [UInt8] {
    var out: [UInt8] = []
    for scalar in text.unicodeScalars {
        let bytes = Array(String(scalar).utf8)
        if out.count + bytes.count > maxBytes { break }
        out.append(contentsOf: bytes)
    }
    return out
}

// Exactly the frames RemoteCore's encodeStatusRow produces. An empty board is
// one frame with count 0.
func statusFrames(board: UInt8, rows: [StatusRow]) -> [Data] {
    let shown = Array(rows.prefix(Wire.statusRowsMax))
    if shown.isEmpty { return [Data([Wire.statusVersion, board, 0, 0, 0, 0, 0])] }
    var frames: [Data] = []
    for (index, row) in shown.enumerated() {
        let title = utf8Prefix(row.title, Wire.statusTitleMax)
        let detail = utf8Prefix(row.detail, Wire.statusDetailMax)
        var bytes: [UInt8] = [Wire.statusVersion, board, UInt8(shown.count), UInt8(index), row.status.rawValue,
                              UInt8(title.count)]
        bytes.append(contentsOf: title)
        bytes.append(UInt8(detail.count))
        bytes.append(contentsOf: detail)
        frames.append(Data(bytes))
    }
    return frames
}

func supportDirectory() -> URL {
    let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("CrossPlayUnlock", isDirectory: true)
    try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
    return base
}

// Runs a program and returns its exit status and standard output. Used for
// the checks and for walking up to the Claude process from a hook.
@discardableResult
func runTool(_ path: String, _ arguments: [String], timeout: TimeInterval = 5) -> (status: Int32, output: String) {
    guard FileManager.default.isExecutableFile(atPath: path) else { return (-1, "") }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: path)
    process.arguments = arguments
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = FileHandle.nullDevice
    do { try process.run() } catch { return (-1, "") }
    let deadline = Date().addingTimeInterval(timeout)
    while process.isRunning && Date() < deadline { usleep(20_000) }
    if process.isRunning {
        process.terminate()
        return (-1, "")
    }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
}

func processAlive(_ pid: Int32) -> Bool {
    if pid <= 0 { return false }
    return kill(pid, 0) == 0 || errno == EPERM
}

// MARK: Claude Code sessions (page 2)
//
// Claude Code runs a hook command on each event, with the event as JSON on
// standard input. `claude-setup` installs `crossplay-unlock claude-hook` for
// the events that change what a session is doing, and the hook records it in
// claude-sessions.json. The agent reads that file; nothing here talks to
// Claude Code any other way.
//
//   UserPromptSubmit, PostToolUse   -> in process
//   PreToolUse on AskUserQuestion
//     or ExitPlanMode, Notification
//     asking for permission         -> awaiting input
//   Stop                            -> completed
//   SessionEnd                      -> removed
//   the claude process gone while
//     in process or awaiting input  -> failed

final class ClaudeSessions {
    static var path: URL { supportDirectory().appendingPathComponent("claude-sessions.json") }
    static var lockPath: String { supportDirectory().appendingPathComponent("claude-sessions.lock").path }

    // Every hook invocation is its own process and several sessions can fire
    // at once, so the file is only ever rewritten under an exclusive lock.
    static func withLock(_ body: () -> Void) {
        let fd = open(lockPath, O_CREAT | O_RDWR, 0o600)
        if fd >= 0 { flock(fd, LOCK_EX) }
        defer {
            if fd >= 0 {
                flock(fd, LOCK_UN)
                close(fd)
            }
        }
        body()
    }

    static func load() -> [String: [String: Any]] {
        guard let data = try? Data(contentsOf: path),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: [String: Any]] else { return [:] }
        return json
    }

    static func save(_ sessions: [String: [String: Any]]) {
        guard let data = try? JSONSerialization.data(withJSONObject: sessions, options: [.prettyPrinted]) else { return }
        let temp = path.appendingPathExtension("part")
        try? data.write(to: temp)
        _ = try? FileManager.default.replaceItemAt(path, withItemAt: temp)
        if !FileManager.default.fileExists(atPath: path.path) { try? data.write(to: path) }
    }

    // The claude process this hook belongs to: the nearest ancestor whose name
    // says claude, else the grandparent (the shell's parent).
    static func claudePid() -> Int32 {
        var pid = getppid()
        var fallback: Int32 = 0
        for depth in 0..<6 {
            let info = runTool("/bin/ps", ["-o", "ppid=,comm=", "-p", String(pid)], timeout: 2).output
                .trimmingCharacters(in: .whitespacesAndNewlines)
            guard let space = info.firstIndex(of: " ") else { break }
            let parent = Int32(info[..<space].trimmingCharacters(in: .whitespaces)) ?? 0
            let name = info[space...].trimmingCharacters(in: .whitespaces).lowercased()
            if name.contains("claude") { return pid }
            if depth == 1 { fallback = pid }
            if parent <= 1 { break }
            pid = parent
        }
        return fallback
    }

    // A session's title as Claude Code wrote it, from the transcript's summary
    // line when there is one.
    static func summary(from transcript: String?) -> String? {
        guard let transcript = transcript, let data = FileManager.default.contents(atPath: transcript),
              let text = String(data: data, encoding: .utf8) else { return nil }
        var found: String?
        for line in text.split(separator: "\n") where line.contains("\"summary\"") {
            if let json = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
               json["type"] as? String == "summary", let summary = json["summary"] as? String, !summary.isEmpty {
                found = summary
            }
        }
        return found
    }

    // `crossplay-unlock claude-hook`: never prints, never fails. Anything on
    // standard output from UserPromptSubmit would be added to Claude's context.
    static func hook() {
        let input = FileHandle.standardInput.readDataToEndOfFile()
        guard let event = try? JSONSerialization.jsonObject(with: input) as? [String: Any],
              let id = event["session_id"] as? String, let name = event["hook_event_name"] as? String else { return }
        let now = Date().timeIntervalSince1970
        withLock {
            var sessions = load()
            var entry = sessions[id] ?? [:]
            if let cwd = event["cwd"] as? String { entry["cwd"] = cwd }
            switch name {
            case "UserPromptSubmit":
                entry["status"] = "inProcess"
                if (entry["firstPrompt"] as? String ?? "").isEmpty, let prompt = event["prompt"] as? String {
                    entry["firstPrompt"] = String(prompt.prefix(200))
                }
                if (entry["pid"] as? Int ?? 0) == 0 { entry["pid"] = Int(claudePid()) }
            case "PostToolUse":
                entry["status"] = "inProcess"
            case "PreToolUse":
                entry["status"] = "awaitingInput"
            case "Notification":
                let message = (event["message"] as? String ?? "").lowercased()
                // Claude also notifies when it has sat idle after finishing;
                // that is not a question, so a completed turn stays completed.
                if message.contains("permission") || (entry["status"] as? String) == "inProcess" {
                    entry["status"] = "awaitingInput"
                }
            case "Stop":
                entry["status"] = "completed"
                if let title = summary(from: event["transcript_path"] as? String) { entry["title"] = title }
            case "SessionEnd":
                sessions.removeValue(forKey: id)
                save(sessions)
                return
            default:
                return
            }
            entry["updated"] = now
            sessions[id] = entry
            save(sessions)
        }
    }

    // The board the agent sends: the ones wanting a person first, then work in
    // progress, then the finished, newest first within each.
    static func rows() -> [StatusRow] {
        let now = Date().timeIntervalSince1970
        var changed = false
        var rows: [(rank: Int, updated: Double, row: StatusRow)] = []
        withLock {
            var sessions = load()
            for (id, entry) in sessions {
                let updated = entry["updated"] as? Double ?? 0
                var status = entry["status"] as? String ?? ""
                let pid = Int32(entry["pid"] as? Int ?? 0)
                if (status == "inProcess" || status == "awaitingInput") && pid > 0 && !processAlive(pid) {
                    status = "failed"
                    var next = entry
                    next["status"] = status
                    next["updated"] = now
                    sessions[id] = next
                    changed = true
                }
                // Finished work drops off after a day, failures after two hours.
                let age = now - updated
                if (status == "completed" && age > 86_400) || (status == "failed" && age > 7_200) || status.isEmpty {
                    sessions.removeValue(forKey: id)
                    changed = true
                    continue
                }
                let code: StatusCode
                let rank: Int
                switch status {
                case "awaitingInput": code = .awaitingInput; rank = 0
                case "failed": code = .failed; rank = 1
                case "inProcess": code = .inProcess; rank = 2
                default: code = .completed; rank = 3
                }
                let cwd = entry["cwd"] as? String ?? ""
                let folder = cwd.isEmpty ? "" : URL(fileURLWithPath: cwd).lastPathComponent
                var title = entry["title"] as? String ?? ""
                if title.isEmpty { title = (entry["firstPrompt"] as? String ?? "").replacingOccurrences(of: "\n", with: " ") }
                if title.isEmpty { title = folder.isEmpty ? "Claude Code" : folder }
                rows.append((rank: rank, updated: updated, row: StatusRow(status: code, title: title, detail: folder)))
            }
            if changed { save(sessions) }
        }
        return rows.sorted { $0.rank != $1.rank ? $0.rank < $1.rank : $0.updated > $1.updated }.map { $0.row }
    }

    // `crossplay-unlock claude-setup`: adds the hook to ~/.claude/settings.json,
    // keeping everything already there and a copy of the file as it was.
    static func setup() {
        let settingsURL = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".claude/settings.json")
        var settings: [String: Any] = [:]
        if let data = try? Data(contentsOf: settingsURL) {
            guard let parsed = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                print("~/.claude/settings.json is not valid JSON; not touching it. Fix it and run this again.")
                exit(1)
            }
            settings = parsed
            let backup = settingsURL.appendingPathExtension("crossplay-backup")
            try? FileManager.default.removeItem(at: backup)
            try? FileManager.default.copyItem(at: settingsURL, to: backup)
        }
        let installed = "/usr/local/bin/crossplay-unlock"
        let binary = FileManager.default.isExecutableFile(atPath: installed)
            ? installed : URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL.path
        let command = "\(binary) claude-hook"
        var hooks = settings["hooks"] as? [String: Any] ?? [:]
        let wanted: [(event: String, matcher: String?)] = [
            ("UserPromptSubmit", nil), ("PreToolUse", "AskUserQuestion|ExitPlanMode"), ("PostToolUse", "*"),
            ("Notification", nil), ("Stop", nil), ("SessionEnd", nil),
        ]
        var added = 0
        for (event, matcher) in wanted {
            var groups = hooks[event] as? [[String: Any]] ?? []
            let already = groups.contains { group in
                (group["hooks"] as? [[String: Any]] ?? []).contains {
                    ($0["command"] as? String ?? "").contains("crossplay-unlock claude-hook")
                }
            }
            if already { continue }
            var group: [String: Any] = ["hooks": [["type": "command", "command": command]]]
            if let matcher = matcher { group["matcher"] = matcher }
            groups.append(group)
            hooks[event] = groups
            added += 1
        }
        settings["hooks"] = hooks
        try? FileManager.default.createDirectory(at: settingsURL.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        guard let data = try? JSONSerialization.data(withJSONObject: settings, options: [.prettyPrinted, .sortedKeys]),
              (try? data.write(to: settingsURL)) != nil else {
            print("could not write ~/.claude/settings.json")
            exit(1)
        }
        print(added == 0 ? "Already set up." : "Added the CrossPlay hook to \(added) Claude Code events.")
        print("Sessions started from now on appear on the reader's CLAUDE page.")
    }
}

// MARK: Mac services (page 3)
//
// services.txt, beside the ledger: one service per line, NAME | CHECK.

struct ServiceLine {
    var name: String
    var check: String
    var start: String  // the third field, "" when the line has none
}

// FNV-1a over the bytes the reader was sent for this title, low byte: what
// the reader echoes with a restart so a list that moved cannot restart the
// wrong row.
func serviceCheck(_ title: String) -> UInt8 {
    var hash: UInt32 = 2166136261
    for byte in utf8Prefix(title, Wire.statusTitleMax) {
        hash ^= UInt32(byte)
        hash = hash &* 16777619
    }
    return UInt8(hash & 0xFF)
}

final class Services {
    static var path: URL { supportDirectory().appendingPathComponent("services.txt") }
    static var logs: URL {
        let dir = supportDirectory().appendingPathComponent("logs", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    static let sample = """
    # CrossPlay Remote, page 3: one service per line.   NAME | CHECK | START
    #
    # CHECK is one of:
    #   launchd <label>      a launchd job:    launchd com.example.agent
    #   process <text>       a running process whose command line contains <text>
    #   docker <container>   a Docker container
    #   http <url>           something answering over HTTP
    #   self                 this helper
    #
    # START is optional: the shell command that starts it again, run when you
    # tap RESTART on the reader. launchd and docker lines need none (it is
    # `launchctl kickstart` / `docker start`). If the restart does not bring it
    # back, Claude Code looks into it in the Service doctor chat.
    #
    # Better than a restart button: `crossplay-unlock adopt` makes macOS keep a
    # service running by itself. See the README.
    #
    # These are guesses. Change each CHECK to match how it runs on this Mac;
    # `crossplay-unlock services` prints what each line finds right now.

    Ambient tasks | process ambient
    Immich        | http http://localhost:2283/api/server/ping
    Voice typing  | process voice
    Wake TV       | process wake
    Remote unlock | self

    """

    static func lines() -> [ServiceLine] {
        if !FileManager.default.fileExists(atPath: path.path) {
            try? sample.data(using: .utf8)?.write(to: path)
        }
        guard let text = try? String(contentsOf: path, encoding: .utf8) else { return [] }
        var out: [ServiceLine] = []
        for raw in text.split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let parts = line.split(separator: "|", maxSplits: 2).map { $0.trimmingCharacters(in: .whitespaces) }
            if parts.count >= 2, !parts[0].isEmpty {
                out.append(ServiceLine(name: parts[0], check: parts[1], start: parts.count > 2 ? parts[2] : ""))
            }
        }
        return out
    }

    // Rewrites the line named `name` (or appends one), keeping every other
    // line and comment exactly as the owner wrote it.
    static func setLine(_ name: String, _ check: String, _ start: String) {
        _ = lines()  // writes the sample when there is no file yet
        let text = (try? String(contentsOf: path, encoding: .utf8)) ?? ""
        var out: [String] = []
        var replaced = false
        let wanted = start.isEmpty ? "\(name) | \(check)" : "\(name) | \(check) | \(start)"
        for raw in text.components(separatedBy: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            let first = line.split(separator: "|", maxSplits: 1).first.map { $0.trimmingCharacters(in: .whitespaces) }
            if !line.hasPrefix("#"), first == name {
                if !replaced { out.append(wanted) }
                replaced = true
                continue
            }
            out.append(raw)
        }
        if !replaced {
            while out.last == "" { out.removeLast() }
            out.append(wanted)
            out.append("")
        }
        try? out.joined(separator: "\n").data(using: .utf8)?.write(to: path)
    }

    static func removeLine(_ name: String) {
        guard let text = try? String(contentsOf: path, encoding: .utf8) else { return }
        let kept = text.components(separatedBy: "\n").filter { raw in
            let line = raw.trimmingCharacters(in: .whitespaces)
            let first = line.split(separator: "|", maxSplits: 1).first.map { $0.trimmingCharacters(in: .whitespaces) }
            return line.hasPrefix("#") || first != name
        }
        try? kept.joined(separator: "\n").data(using: .utf8)?.write(to: path)
    }

    static func docker() -> String? {
        ["/usr/local/bin/docker", "/opt/homebrew/bin/docker", "/Applications/Docker.app/Contents/Resources/bin/docker"]
            .first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    // How a line is started again: its own START, else what its CHECK implies.
    static func recipe(_ line: ServiceLine) -> String? {
        if !line.start.isEmpty { return line.start }
        let parts = line.check.split(separator: " ", maxSplits: 1).map(String.init)
        let arg = parts.count > 1 ? parts[1].trimmingCharacters(in: .whitespaces) : ""
        switch parts.first?.lowercased() ?? "" {
        case "launchd":
            let plist = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/LaunchAgents/\(arg).plist").path
            // kickstart restarts a loaded job; bootstrap loads one that fell out.
            return "/bin/launchctl kickstart -k gui/\(getuid())/\(arg) || /bin/launchctl bootstrap gui/\(getuid()) '\(plist)'"
        case "docker":
            guard let docker = docker() else { return nil }
            return "'\(docker)' start \(arg)"
        default:
            return nil
        }
    }

    static func check(_ check: String) -> (StatusCode, String) {
        let parts = check.split(separator: " ", maxSplits: 1).map(String.init)
        let kind = parts.first?.lowercased() ?? ""
        let arg = parts.count > 1 ? parts[1].trimmingCharacters(in: .whitespaces) : ""
        switch kind {
        case "self":
            return (.running, "this helper")
        case "process":
            let found = runTool("/usr/bin/pgrep", ["-f", arg]).status == 0
            return (found ? .running : .stopped, "process")
        case "launchd":
            var result = runTool("/bin/launchctl", ["print", "gui/\(getuid())/\(arg)"])
            if result.status != 0 { result = runTool("/bin/launchctl", ["print", "system/\(arg)"]) }
            if result.status != 0 { return (.unknown, "not loaded") }
            if result.output.contains("state = running") { return (.running, "launchd") }
            if let range = result.output.range(of: "last exit code = ") {
                let code = result.output[range.upperBound...].prefix { $0.isNumber || $0 == "-" }
                if let value = Int(code), value != 0 { return (.failed, "exit \(value)") }
            }
            return (.stopped, "launchd")
        case "docker":
            guard let docker = docker() else { return (.unknown, "no docker") }
            let state = runTool(docker, ["inspect", "-f", "{{.State.Status}} {{.State.ExitCode}}", arg])
            if state.status != 0 { return (.unknown, "docker off") }
            let words = state.output.split(separator: " ").map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            if words.first == "running" { return (.running, "docker") }
            if words.count > 1, let code = Int(words[1]), code != 0 { return (.failed, "exit \(code)") }
            return (.stopped, "docker")
        case "http":
            guard let url = URL(string: arg) else { return (.unknown, "bad url") }
            var request = URLRequest(url: url)
            request.timeoutInterval = 4
            let done = DispatchSemaphore(value: 0)
            var answer: (StatusCode, String) = (.stopped, "no answer")
            URLSession.shared.dataTask(with: request) { _, response, error in
                if let http = response as? HTTPURLResponse {
                    answer = http.statusCode < 500 ? (.running, "http") : (.failed, "http \(http.statusCode)")
                } else if error != nil {
                    answer = (.stopped, "no answer")
                }
                done.signal()
            }.resume()
            _ = done.wait(timeout: .now() + 5)
            return answer
        default:
            return (.unknown, "bad check")
        }
    }

    // What a restart in progress, or its result, says in place of the check.
    struct Override {
        var status: StatusCode
        var detail: String
        var until: Date
    }
    private static var overrides: [String: Override] = [:]
    private static let overrideLock = NSLock()

    static func setOverride(_ name: String, _ status: StatusCode, _ detail: String, for seconds: TimeInterval) {
        overrideLock.lock()
        overrides[name] = Override(status: status, detail: detail, until: Date().addingTimeInterval(seconds))
        overrideLock.unlock()
    }

    static func clearOverride(_ name: String) {
        overrideLock.lock()
        overrides.removeValue(forKey: name)
        overrideLock.unlock()
    }

    static func rows() -> [StatusRow] {
        lines().map { line in
            let (status, detail) = check(line.check)
            overrideLock.lock()
            var override = overrides[line.name]
            if let o = override, o.until < Date() {
                overrides.removeValue(forKey: line.name)
                override = nil
            }
            overrideLock.unlock()
            if let o = override {
                // A result line (Claude's answer) gives way to the real state
                // once it expires; a restart in progress always shows.
                return StatusRow(status: o.status, title: line.name, detail: o.detail)
            }
            return StatusRow(status: status, title: line.name, detail: detail)
        }
    }

    // RESTART from the reader. One at a time, off the main thread: the
    // recipe first, and the Service doctor only when that did not work.
    static let restartQueue = DispatchQueue(label: "crossplay.restart")

    static func restart(row: Int, check: UInt8, changed: @escaping () -> Void) {
        let all = lines()
        guard row >= 0, row < min(all.count, Wire.statusRowsMax) else {
            log("restart: no row \(row)")
            return
        }
        let line = all[row]
        guard serviceCheck(line.name) == check else {
            log("restart: row \(row) is no longer the one the reader showed; refused")
            return
        }
        restartQueue.async {
            log("restart: \(line.name)")
            Services.setOverride(line.name, .inProcess, "restarting", for: 600)
            DispatchQueue.main.async(execute: changed)
            var printed = ""
            let startCommand = Services.recipe(line)
            if let startCommand = startCommand {
                let result = runTool("/bin/sh", ["-c", "\(startCommand) 2>&1"], timeout: 60)
                printed = String(result.output.suffix(1500))
                log("restart: recipe exited \(result.status)")
            }
            sleep(8)
            let (after, afterDetail) = Services.check(line.check)
            if after == .running {
                Services.setOverride(line.name, .running, "restarted", for: 120)
                DispatchQueue.main.async(execute: changed)
                return
            }
            Services.setOverride(line.name, .inProcess, "asking Claude", for: 900)
            DispatchQueue.main.async(execute: changed)
            let answer = Doctor.ask(about: line, recipe: startCommand, printed: printed,
                                    state: "\(after) (\(afterDetail))")
            let (settled, _) = Services.check(line.check)
            Services.setOverride(line.name, settled == .running ? .running : .failed, answer, for: 1800)
            DispatchQueue.main.async(execute: changed)
        }
    }
}

// MARK: The Service doctor
//
// Claude Code, run headless on this Mac when a restart did not bring a
// service back. Every call resumes ONE session, so all of it is a single chat
// ("Service doctor") that remembers what it tried last time, and it shows on
// the reader's CLAUDE page like any other session. `claude --resume <id>` in
// a terminal opens the same conversation.
enum Doctor {
    static var folder: URL {
        let dir = supportDirectory().appendingPathComponent("doctor", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
    static var sessionFile: URL { supportDirectory().appendingPathComponent("doctor-session.txt") }
    static var toolsFile: URL { supportDirectory().appendingPathComponent("doctor-tools.txt") }
    static var logFile: URL { Services.logs.appendingPathComponent("doctor.log") }

    // What Claude may do without asking, since nobody is at the Mac to be
    // asked. Looking, and starting and stopping services; nothing else.
    // doctor-tools.txt, one rule per line, replaces this list.
    static let defaultTools = [
        "Read", "Grep", "Glob",
        "Bash(launchctl:*)", "Bash(docker:*)", "Bash(open:*)", "Bash(ps:*)", "Bash(pgrep:*)", "Bash(pkill:*)",
        "Bash(lsof:*)", "Bash(tail:*)", "Bash(cat:*)", "Bash(ls:*)", "Bash(log show:*)", "Bash(brew services:*)",
        "Bash(crossplay-unlock:*)", "Bash(curl:*)",
    ]

    static func tools() -> [String] {
        guard let text = try? String(contentsOf: toolsFile, encoding: .utf8) else { return defaultTools }
        let rules = text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty && !$0.hasPrefix("#") }
        return rules.isEmpty ? defaultTools : rules
    }

    static func claudePath() -> String? {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let candidates = ["\(home)/.local/bin/claude", "\(home)/.claude/local/claude", "/opt/homebrew/bin/claude",
                          "/usr/local/bin/claude"]
        if let found = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) { return found }
        let viaShell = runTool("/bin/zsh", ["-lc", "command -v claude"], timeout: 10).output
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return viaShell.isEmpty ? nil : viaShell
    }

    static func run(_ claude: String, _ arguments: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: claude)
        process.arguments = arguments
        process.currentDirectoryURL = folder
        var environment = ProcessInfo.processInfo.environment
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        environment["PATH"] = "\(home)/.local/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        environment["HOME"] = home
        process.environment = environment
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        process.standardInput = FileHandle.nullDevice
        do { try process.run() } catch { return (-1, "could not start claude") }
        // Read as it arrives, so a long answer cannot fill the pipe and stall it.
        var data = Data()
        let reader = DispatchQueue(label: "crossplay.doctor.read")
        let done = DispatchSemaphore(value: 0)
        reader.async {
            data = pipe.fileHandleForReading.readDataToEndOfFile()
            done.signal()
        }
        let deadline = Date().addingTimeInterval(600)
        while process.isRunning && Date() < deadline { usleep(200_000) }
        if process.isRunning { process.terminate() }
        _ = done.wait(timeout: .now() + 5)
        return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
    }

    static func ask(about line: ServiceLine, recipe: String?, printed: String, state: String) -> String {
        guard let claude = claudePath() else {
            log("doctor: claude is not installed where this helper can find it")
            return "claude not found"
        }
        let prompt = """
        You are the Service doctor for this Mac, run from the CrossPlay reader's RESTART button. Nobody is at \
        the Mac to answer questions, so do not ask any.

        The service "\(line.name)" is not running.
        - How it is checked: \(line.check)
        - How it is normally started: \(recipe ?? "not recorded")
        - What that start command printed just now: \(printed.isEmpty ? "(nothing)" : printed)
        - What the check says now: \(state)
        - The services list is \(Services.path.path) (NAME | CHECK | START).

        Find out why it is down and get it running again. Touch nothing unrelated to this service. If it needs \
        a start command the list does not have, say what it should be.

        End your reply with ONE line of at most 30 characters saying what you did or what is wrong; the \
        reader shows only that line.
        """
        let started = FileManager.default.fileExists(atPath: sessionFile.path)
        var id = (try? String(contentsOf: sessionFile, encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if id.isEmpty { id = UUID().uuidString.lowercased() }
        let common = ["-p", prompt, "--output-format", "text", "--allowedTools"] + tools()
        var result = run(claude, common + (started ? ["--resume", id] : ["--session-id", id]))
        if result.status != 0 && started {
            // The saved session is gone (cleared, or another machine's). Start
            // the chat again under a new id rather than failing every restart.
            log("doctor: could not resume \(id); starting a new chat")
            id = UUID().uuidString.lowercased()
            result = run(claude, common + ["--session-id", id])
        }
        if result.status == 0 { try? id.data(using: .utf8)?.write(to: sessionFile) }
        let stamp = ISO8601DateFormatter().string(from: Date())
        let entry = "\n== \(stamp) \(line.name) (exit \(result.status))\n\(result.output)\n"
        if let handle = try? FileHandle(forWritingTo: logFile) {
            handle.seekToEndOfFile()
            handle.write(Data(entry.utf8))
            try? handle.close()
        } else {
            try? Data(entry.utf8).write(to: logFile)
        }
        let last = result.output.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }
            .last { !$0.isEmpty } ?? ""
        if result.status != 0 { return "Claude failed (exit \(result.status))" }
        return last.isEmpty ? "Claude had nothing to say" : String(last.prefix(Wire.statusDetailMax))
    }
}

// MARK: Keeping services alive (`adopt`)
//
// The first line of defence: a service macOS itself keeps running, so the
// restart button is rarely needed. `adopt` makes a launchd agent with
// KeepAlive for a command or an app, or tells Docker to restart a container,
// and points the service's line in services.txt at it.
enum Adopt {
    static func slug(_ name: String) -> String {
        let allowed = name.lowercased().map { $0.isLetter || $0.isNumber ? $0 : "-" }
        return String(allowed).split(separator: "-").joined(separator: "-")
    }

    static func label(_ name: String) -> String { "com.crossplay.svc.\(slug(name))" }

    static func plistURL(_ label: String) -> URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }

    static func agent(name: String, arguments: [String]) {
        let label = label(name)
        let url = plistURL(label)
        let logPath = Services.logs.appendingPathComponent("\(slug(name)).log").path
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let plist: [String: Any] = [
            "Label": label,
            "ProgramArguments": arguments,
            "RunAtLoad": true,
            // Restarted whenever it exits, for any reason -- that is the point.
            "KeepAlive": true,
            // At most one restart every 20 seconds, so a service that dies on
            // launch does not spin.
            "ThrottleInterval": 20,
            "StandardOutPath": logPath,
            "StandardErrorPath": logPath,
            "EnvironmentVariables": ["PATH": "\(home)/.local/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"],
        ]
        guard let data = try? PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0) else {
            print("could not write the launchd file")
            exit(1)
        }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        runTool("/bin/launchctl", ["bootout", "gui/\(getuid())/\(label)"])
        do { try data.write(to: url) } catch {
            print("could not write \(url.path)")
            exit(1)
        }
        let loaded = runTool("/bin/launchctl", ["bootstrap", "gui/\(getuid())", url.path])
        if loaded.status != 0 { print("launchctl could not load it (status \(loaded.status)); see \(logPath)") }
        Services.setLine(name, "launchd \(label)", "")
        print("\(name) now runs as \(label) and macOS restarts it whenever it stops.")
        print("log: \(logPath)")
    }

    static func command(_ args: [String]) {
        guard args.count >= 2 else { usage() }
        let name = args[0]
        switch args[1] {
        case "--run" where args.count >= 3:
            // Through a login shell, so the command sees the PATH it sees in
            // Terminal. It must stay in the foreground: a command that forks
            // and exits looks like a crash to KeepAlive, and is restarted.
            agent(name: name, arguments: ["/bin/zsh", "-lc", args[2...].joined(separator: " ")])
        case "--app" where args.count >= 3:
            // open -W waits for the app to quit, so launchd sees it as running.
            agent(name: name, arguments: ["/usr/bin/open", "-W", "-a", args[2...].joined(separator: " ")])
        case "--docker" where args.count >= 3:
            guard let docker = Services.docker() else {
                print("docker was not found")
                exit(1)
            }
            let result = runTool(docker, ["update", "--restart", "unless-stopped", args[2]])
            if result.status != 0 {
                print("docker could not update \(args[2]); is Docker running and is that the container's name?")
                exit(1)
            }
            Services.setLine(name, "docker \(args[2])", "")
            print("\(name): Docker now restarts \(args[2]) whenever it stops (and when Docker starts).")
        case "--launchd" where args.count >= 3:
            Services.setLine(name, "launchd \(args[2])", "")
            print("\(name) is checked and restarted as the launchd job \(args[2]).")
        default:
            usage()
        }
    }

    static func remove(_ name: String) {
        let label = label(name)
        runTool("/bin/launchctl", ["bootout", "gui/\(getuid())/\(label)"])
        try? FileManager.default.removeItem(at: plistURL(label))
        Services.removeLine(name)
        print("\(name) is no longer kept alive, and is off the MAC page.")
    }

    static func usage() -> Never {
        print("""
        usage:
          crossplay-unlock adopt NAME --run "COMMAND"     keep a command running (it must stay in the foreground)
          crossplay-unlock adopt NAME --app "App Name"    keep an app open
          crossplay-unlock adopt NAME --docker CONTAINER  have Docker restart a container
          crossplay-unlock adopt NAME --launchd LABEL     watch and restart an existing launchd job
          crossplay-unlock unadopt NAME
        """)
        exit(2)
    }
}

// MARK: - The agent

final class Agent: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var reader: CBPeripheral?
    private var responseCharacteristic: CBCharacteristic?
    private var nowPlayingCharacteristic: CBCharacteristic?
    private var macStateCharacteristic: CBCharacteristic?
    private var statusCharacteristic: CBCharacteristic?
    // What each board last sent; nil after a reconnect, so the reader is told
    // everything again.
    private var sentBoards: [UInt8: [Data]] = [:]
    private var claudeTimer: Timer?
    private var servicesTimer: Timer?
    private var servicesRows: [StatusRow]?
    private let checkQueue = DispatchQueue(label: "crossplay.services")
    private let microphones = Microphones()
    private var lastMacState: Data?
    private let ledger = Ledger()

    private var heard: [String: NowPlaying.Heard] = [:]
    private var observers: [NSObjectProtocol] = []
    // What the reader was last sent. nil means "send whatever is current the
    // next time there is somewhere to send it", which is the state after every
    // reconnect: the reader forgets the song when its radio goes down.
    private var lastSent: Data?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
        microphones.onChange = { [weak self] in self?.sendMacState() }
        // Claude sessions change on a hook, so the file is cheap to look at
        // often; the services checks spawn processes, so they run less often
        // and off the main thread.
        claudeTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.sendBoard(Wire.boardClaude, ClaudeSessions.rows())
        }
        servicesTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            self?.refreshServices()
        }
        refreshServices()
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

    private func refreshServices() {
        checkQueue.async { [weak self] in
            let rows = Services.rows()
            DispatchQueue.main.async {
                self?.servicesRows = rows
                self?.sendBoard(Wire.boardServices, rows)
            }
        }
    }

    // A board goes out only when it changed, row by row, with responses so a
    // failed write is logged and the board is sent again next time.
    private func sendBoard(_ board: UInt8, _ rows: [StatusRow]) {
        let frames = statusFrames(board: board, rows: rows)
        guard frames != sentBoards[board] else { return }
        guard let characteristic = statusCharacteristic, let peripheral = reader else { return }
        for frame in frames { peripheral.writeValue(frame, for: characteristic, type: .withResponse) }
        sentBoards[board] = frames
    }

    private func handleCommand(_ frame: Data) {
        let bytes = [UInt8](frame)
        if bytes.count == 4, bytes[0] == Wire.macLinkVersion, bytes[1] == Wire.commandRestart {
            Services.restart(row: Int(bytes[2]), check: bytes[3]) { [weak self] in self?.refreshServices() }
            return
        }
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
        if characteristic.uuid == Wire.status { sentBoards = [:] }
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
        statusCharacteristic = nil
        sentBoards = [:]
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
                                            Wire.macState, Wire.status], for: service)
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
            if characteristic.uuid == Wire.status {
                statusCharacteristic = characteristic
                sentBoards = [:]
                sendBoard(Wire.boardClaude, ClaudeSessions.rows())
                if let rows = servicesRows { sendBoard(Wire.boardServices, rows) }
            }
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
        // The secret is read from the Keychain for EVERY request, never once at
        // startup. `pair` is a separate process that replaces it while this one
        // runs; a copy held from launch answered every request after a re-pair
        // as a stranger's, silently, until ten of them locked the helper out.
        guard let secret = Store.get("secret") else {
            log("not paired; ignoring. Run: crossplay-unlock pair")
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
    // Every well-formed code decodes to SOME secret, so a mistyped character
    // is not an error here -- it is a pairing that silently never answers.
    // Showing back what was understood is the one place it can be caught.
    print("")
    print("Check this matches the reader, group by group:")
    print("  \(Base32.encode(secret))")
    print("If any group differs, run crossplay-unlock pair again.")
    print("")
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
    // Runs unpaired too, and picks the pairing up when `pair` stores one:
    // exiting here would have launchd restart it every ten seconds forever.
    if Store.get("secret") == nil { log("not paired yet. Run: crossplay-unlock pair") }
    runningAgent = Agent()
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
case "claude-hook": ClaudeSessions.hook()
case "claude-setup": ClaudeSessions.setup()
case "adopt": Adopt.command(Array(CommandLine.arguments.dropFirst(2)))
case "unadopt":
    guard CommandLine.arguments.count >= 3 else { Adopt.usage() }
    Adopt.remove(CommandLine.arguments[2])
case "doctor":
    let id = ((try? String(contentsOf: Doctor.sessionFile, encoding: .utf8)) ?? "")
        .trimmingCharacters(in: .whitespacesAndNewlines)
    print(id.isEmpty ? "The Service doctor has not been asked anything yet." : "Service doctor chat: claude --resume \(id)")
    print("log: \(Doctor.logFile.path)")
case "services":
    for line in Services.lines() {
        let (status, detail) = Services.check(line.check)
        print("\(line.name): \(status) (\(detail))  [\(line.check)]")
    }
    print("edit: \(Services.path.path)")
default:
    print("usage: crossplay-unlock [pair|password|unblock|status|forget|run|claude-setup|services|adopt|unadopt|doctor]")
    exit(2)
}
