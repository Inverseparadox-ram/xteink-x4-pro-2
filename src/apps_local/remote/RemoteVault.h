#pragma once

// The unlock button's cryptography and wire format.
//
// Freestanding C++17 -- no NimBLE, no mbedTLS, no Arduino -- so
// host-tests/remotevault builds it with a bare compiler and checks the MAC
// against RFC 4231's published vectors and the key derivation against RFC
// 7914's. That is the whole reason SHA-256 is reimplemented here rather than
// called from the ESP-IDF: a MAC nobody can run on a host is a MAC nobody can
// prove, and "it compiled" is not a proof.
//
// ---------------------------------------------------------------------------
// WHAT THIS CAN AND CANNOT DO. Read this before trusting it.
//
// macOS exposes NO API for dismissing the lock screen. Not to a signed helper,
// not to a LaunchAgent, not to anything: Apple reserves unlocking for the
// password field, Touch ID and Apple Watch. So the only way any Bluetooth
// device unlocks a Mac is to TYPE THE PASSWORD, and that is what this does --
// over the same HID keyboard link the media buttons already use.
//
// What the exchange below buys, given that the keystrokes ARE the unlock:
//
//   1. THE READER HOLDS NO PASSWORD. The password stays in the Mac's Keychain,
//      where a password belongs. The Mac sends it -- encrypted under a key
//      derived from the shared secret and a nonce that has never been used
//      before -- only in answer to a request it has authenticated, and only
//      while its own screen is actually locked. The reader decrypts it into
//      stack memory, types it, and wipes it.
//
//   2. THE READER WILL NOT TYPE INTO A STRANGER'S MAC, because it has nothing
//      to type until a host that holds the secret has answered.
//
//   3. THE READER WILL NOT TYPE INTO AN UNLOCKED SESSION. The host reports
//      whether the screen is locked, inside the MAC so it cannot be forged,
//      and sends no password unless it is. Without this the failure is ugly
//      and silent: press unlock at an awake Mac and the password goes into
//      whatever field has focus -- a chat window, a search box, a shared
//      screen.
//
//   4. A STOLEN READER IS NOT A KEY. The secret is not stored; it is stored
//      SEALED under a PIN the user types, and -- this is the part that
//      matters -- NO VERIFIER IS STORED BESIDE IT. Every PIN yields a
//      well-formed 20-byte secret, so an attacker holding the SD card has
//      nothing to test a guess against offline. The only oracle is the Mac
//      itself, which sees a bad MAC and backs off. See sealSecret().
//
// What it does NOT buy, stated plainly so nobody has to infer it:
//
//   - Anyone who has the reader AND the PIN can unlock the Mac. That is the
//     design, not a flaw in it: it is a key and a code, and it is exactly two
//     factors, one of them a thing you carry.
//   - It does nothing at the FileVault pre-boot screen. Bluetooth is not up
//     that early, so a Mac that has been powered off needs its keyboard.
//   - It does not protect against a compromised Mac. A machine that is already
//     running an attacker's code does not need the reader's help.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

namespace remote {
namespace vault {

// --- SHA-256 and HMAC, so the wire bytes are testable -----------------------

inline constexpr size_t kHashLen = 32;

void sha256(const uint8_t* data, size_t len, uint8_t out[kHashLen]);

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t out[kHashLen]);

// Compares in time independent of where the first difference falls. A plain
// memcmp on a MAC leaks the length of the matching prefix, which is enough to
// forge one a byte at a time.
bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len);

// PBKDF2-HMAC-SHA256, RFC 8018. Here only to stretch the PIN; see sealSecret().
void pbkdf2(const uint8_t* password, size_t passwordLen, const uint8_t* salt, size_t saltLen, uint32_t iterations,
            uint8_t* out, size_t outLen);

// --- The shared secret ------------------------------------------------------

// 20 bytes, because it is typed in by hand exactly once. That is 160 bits --
// far past anything brute-forceable -- and 32 base32 characters, which fits on
// the reader's screen in eight readable groups of four.
inline constexpr size_t kSecretLen = 20;

// Crockford-style base32 without padding, uppercase. `out` needs 33 bytes.
void encodeSecret(const uint8_t secret[kSecretLen], char* out, size_t size);

// Accepts the groups with or without separators, and folds the characters
// people actually mistype: O for 0, I and L for 1. Returns false on any
// character it cannot place or a length that is not 32.
bool decodeSecret(const char* text, uint8_t out[kSecretLen]);

// --- Sealing the secret under a PIN -----------------------------------------

inline constexpr size_t kSaltLen = 16;
inline constexpr size_t kPinMinLen = 4;
inline constexpr size_t kPinMaxLen = 12;

// Chosen so a wrong guess costs about a second on the reader's own CPU, which
// is invisible next to the e-ink refresh that follows it and is the entire
// budget an attacker gets per guess -- because guessing offline is the thing
// this design removes.
inline constexpr uint32_t kPinIterations = 20000;

// XOR, deliberately, and this is the whole trick: a sealed secret is
// INDISTINGUISHABLE FROM RANDOM under every PIN. There is no tag to check, no
// padding to be wrong, no checksum to match -- so an attacker holding the
// sealed bytes and the salt cannot tell a right guess from a wrong one without
// asking the Mac, and the Mac counts. A sealed format with an integrity tag
// would be the more conventional choice and would hand them an offline oracle
// for a four-digit secret.
//
// It follows that this function cannot fail and openSecret cannot detect a bad
// PIN. A wrong PIN produces a wrong secret, the Mac answers with BadMac, and
// that is the only place the mistake can surface.
void sealSecret(const uint8_t secret[kSecretLen], const char* pin, const uint8_t salt[kSaltLen],
                uint8_t out[kSecretLen]);

void openSecret(const uint8_t sealed[kSecretLen], const char* pin, const uint8_t salt[kSaltLen],
                uint8_t out[kSecretLen]);

// Digits only, kPinMinLen..kPinMaxLen of them.
bool pinIsWellFormed(const char* pin);

// --- The exchange -----------------------------------------------------------

inline constexpr uint8_t kProtocolVersion = 1;
inline constexpr size_t kNonceLen = 16;

// The longest password the Mac will send. 96 bytes is past any password a
// person types twice a day, and it keeps the whole response inside a single
// BLE write on a link that has negotiated any MTU worth having.
inline constexpr size_t kMaxSecretPayload = 96;

// What the reader is asking for.
enum class Op : uint8_t {
  Status = 0x01,  // "are you there, and is the screen locked?" -- no password
  Unlock = 0x02,  // "the screen is locked and I am authorised: send it"
};

// What the host says about its screen. Inside the MAC, so it cannot be forged
// by anything that does not hold the secret.
enum class Screen : uint8_t {
  Unknown = 0x00,
  Unlocked = 0x01,
  Locked = 0x02,
};

// There is deliberately no Op::Lock. Locking a Mac is not a security decision
// -- the worst a forged lock can do is lock a screen -- so the lock half of
// the button is a plain keyboard chord that works whether or not the helper is
// running. Only UNLOCK needs the helper to have answered.

// Reader -> host. 58 bytes.
inline constexpr size_t kChallengeLen = 1 + 1 + 8 + kNonceLen + kHashLen;

// Host -> reader: an 11-byte head, the sealed payload, then the MAC.
inline constexpr size_t kResponseHeadLen = 1 + 1 + 8 + 1;
inline constexpr size_t kResponseMaxLen = kResponseHeadLen + kMaxSecretPayload + kHashLen;

struct Challenge {
  uint8_t version = kProtocolVersion;
  Op op = Op::Status;
  // Strictly increasing and persisted on the reader. The host refuses anything
  // it has seen at or below its high-water mark, which is what stops a
  // recorded Unlock request being played back at a locked Mac.
  uint64_t counter = 0;
  uint8_t nonce[kNonceLen] = {};
  uint8_t mac[kHashLen] = {};
};

struct Response {
  uint8_t version = kProtocolVersion;
  Screen screen = Screen::Unknown;
  uint64_t counter = 0;
  uint8_t payloadLen = 0;
  uint8_t payload[kMaxSecretPayload] = {};  // still sealed; see openPayload()
  uint8_t mac[kHashLen] = {};
};

// HMAC(secret, label || nonce). One key per purpose, so the key that
// authenticates a request can never be made to produce a keystream.
void deriveKey(const uint8_t* secret, size_t secretLen, const char* label, const uint8_t nonce[kNonceLen],
               uint8_t out[kHashLen]);

// Fills in `mac` over everything else in the frame.
void signChallenge(const uint8_t* secret, size_t secretLen, Challenge& challenge);

void encodeChallenge(const Challenge& in, uint8_t out[kChallengeLen]);
bool decodeChallenge(const uint8_t* in, size_t len, Challenge& out);

// Host side, kept here so the same code that the tests exercise is the code the
// documented Swift helper has to match byte for byte. Seals `secretText` into
// the response and signs the result.
void buildResponse(const uint8_t* secret, size_t secretLen, const Challenge& challenge, Screen screen,
                   const uint8_t* payload, size_t payloadLen, Response& out);

size_t encodeResponse(const Response& in, uint8_t* out, size_t size);
bool decodeResponse(const uint8_t* in, size_t len, Response& out);

// Why a response was refused. Separated from a bare bool so the screen can say
// which wall it hit -- "no answer" and "wrong answer" are different problems
// and lead to different fixes.
enum class Verdict : uint8_t {
  Ok,
  BadVersion,
  CounterMismatch,  // a reply to a challenge this is not
  BadMac,           // wrong PIN, an unpaired host, or a bent frame
  BadPayload,       // authentic, but longer than anything we will type
};

Verdict verifyResponse(const uint8_t* secret, size_t secretLen, const Challenge& challenge, const Response& response);

// Unseals the payload of a response that verifyResponse has already accepted.
// Writes at most `size - 1` bytes and null-terminates, so the result can go
// straight to the keyboard. Returns the length, or 0 when there is nothing to
// type.
size_t openPayload(const uint8_t* secret, size_t secretLen, const Challenge& challenge, const Response& response,
                   char* out, size_t size);

// Overwrites `len` bytes and does not let the compiler decide the store is
// dead, which is exactly what it decides about a memset to a buffer that is
// about to go out of scope.
void wipe(void* data, size_t len);

// --- What the button does ---------------------------------------------------

// The button is a toggle over a state the reader does NOT own, so it reads
// that state from the last verified response rather than from a local flag. A
// flag would be wrong the first time anyone touched the Mac directly, and
// being wrong here means typing a password at an unlocked screen.
enum class Intent : uint8_t {
  Unlock,  // the screen is locked; ask for the password and type it
  Lock,    // the screen is unlocked; send the lock chord
  Ask,     // nothing verified yet, so neither: find out first
};

Intent intentFor(Screen screen);

// Control-Command-Q, which is macOS's own lock shortcut and needs nothing set
// up. Unlike the Do Not Disturb chord this replaced, it is not a binding the
// user has to create.
struct LockChord {
  uint8_t modifiers;  // 1 LCtrl, 2 LShift, 4 LAlt, 8 LGui
  uint8_t key;        // HID keyboard usage id
};

LockChord lockChord();

}  // namespace vault
}  // namespace remote
