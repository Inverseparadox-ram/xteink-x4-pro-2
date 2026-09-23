// The unlock button's crypto, checked against published vectors and against
// the attacks the design claims to stop.
//
// Two of these tests are the reason the SHA-256 in RemoteVault.cpp exists at
// all rather than a call into mbedTLS: FIPS 180-4 and RFC 4231 pin the exact
// bytes, so a Swift helper written from docs/apps/remote.md and this firmware
// either agree or one of them fails here.

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/apps_local/remote/RemoteVault.h"

namespace vault = remote::vault;

namespace {

int checks = 0;
int failures = 0;

void check(const bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL  %s\n", what);
  }
}

std::string hex(const uint8_t* data, const size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0f]);
  }
  return out;
}

size_t unhex(const char* text, uint8_t* out) {
  size_t n = 0;
  for (const char* p = text; p[0] != '\0' && p[1] != '\0'; p += 2) {
    auto nibble = [](const char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return c - 'A' + 10;
    };
    out[n++] = static_cast<uint8_t>(nibble(p[0]) << 4 | nibble(p[1]));
  }
  return n;
}

// A secret with no structure to it, so a test that accidentally depends on a
// pattern in the key shows up as a failure rather than a pass.
const uint8_t kSecret[vault::kSecretLen] = {0x9c, 0x1e, 0x4a, 0xd0, 0x77, 0x3b, 0xe5, 0x12, 0x80, 0xaf,
                                            0x64, 0x29, 0xfd, 0x05, 0xbb, 0x71, 0x36, 0xc8, 0x9a, 0x4e};

vault::Challenge freshChallenge(const vault::Op op, const uint64_t counter, const uint8_t seed) {
  vault::Challenge challenge;
  challenge.op = op;
  challenge.counter = counter;
  for (size_t i = 0; i < vault::kNonceLen; ++i) {
    challenge.nonce[i] = static_cast<uint8_t>(seed * 31u + i * 7u + 1u);
  }
  vault::signChallenge(kSecret, sizeof(kSecret), challenge);
  return challenge;
}

// --- The primitives ---------------------------------------------------------

void testSha256AgainstPublishedVectors() {
  struct Case {
    const char* input;
    const char* expected;
  };
  static const Case cases[] = {
      // FIPS 180-4's own two, plus the empty string.
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
  };
  for (const Case& one : cases) {
    uint8_t digest[vault::kHashLen];
    vault::sha256(reinterpret_cast<const uint8_t*>(one.input), std::strlen(one.input), digest);
    check(hex(digest, sizeof(digest)) == one.expected, "sha256 matches the published digest");
  }

  // A million 'a', the vector that catches a broken length counter -- the one
  // bug that survives every short input.
  uint8_t digest[vault::kHashLen];
  std::string million(1000000, 'a');
  vault::sha256(reinterpret_cast<const uint8_t*>(million.data()), million.size(), digest);
  check(hex(digest, sizeof(digest)) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "sha256 of a million 'a' matches, so the 64-bit length is fed in correctly");
}

void testHmacAgainstRfc4231() {
  struct Case {
    const char* keyHex;
    const char* dataHex;
    const char* expected;
  };
  static const Case cases[] = {
      // Case 1.
      {"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "4869205468657265",
       "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
      // Case 2: a key shorter than one block.
      {"4a656665", "7768617420646f2079612077616e7420666f72206e6f7468696e673f",
       "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
      // Case 4: a key of counting bytes.
      {"0102030405060708090a0b0c0d0e0f10111213141516171819",
       "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
       "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
      // Case 6: a 131-byte key, which is the ONLY case that exercises the
      // branch where the key is hashed down before padding. Get that branch
      // wrong and every other vector here still passes.
      {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6579204669"
       "727374",
       "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
  };
  for (const Case& one : cases) {
    uint8_t key[256];
    uint8_t data[256];
    const size_t keyLen = unhex(one.keyHex, key);
    const size_t dataLen = unhex(one.dataHex, data);
    uint8_t mac[vault::kHashLen];
    vault::hmacSha256(key, keyLen, data, dataLen, mac);
    check(hex(mac, sizeof(mac)) == one.expected, "hmac-sha256 matches RFC 4231");
  }
}

void testPbkdf2AgainstRfc7914() {
  uint8_t out[64];
  vault::pbkdf2(reinterpret_cast<const uint8_t*>("passwd"), 6, reinterpret_cast<const uint8_t*>("salt"), 4, 1, out,
                sizeof(out));
  check(hex(out, sizeof(out)) ==
            "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
            "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
        "pbkdf2-hmac-sha256 matches RFC 7914 at one iteration and two blocks");

  vault::pbkdf2(reinterpret_cast<const uint8_t*>("Password"), 8, reinterpret_cast<const uint8_t*>("NaCl"), 4, 80000,
                out, sizeof(out));
  check(hex(out, sizeof(out)) ==
            "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
            "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d",
        "pbkdf2-hmac-sha256 matches RFC 7914 at eighty thousand iterations");
}

void testConstantTimeCompareStillCompares() {
  uint8_t a[vault::kHashLen];
  uint8_t b[vault::kHashLen];
  for (size_t i = 0; i < sizeof(a); ++i) a[i] = b[i] = static_cast<uint8_t>(i * 11 + 3);
  check(vault::constantTimeEquals(a, b, sizeof(a)), "equal buffers compare equal");

  // Every single-bit difference at every position. Constant time is worth
  // nothing if the comparison has stopped noticing.
  for (size_t i = 0; i < sizeof(a); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      b[i] = static_cast<uint8_t>(a[i] ^ (1 << bit));
      check(!vault::constantTimeEquals(a, b, sizeof(a)), "a one-bit difference is still a difference");
    }
    b[i] = a[i];
  }
}

// --- The pairing code -------------------------------------------------------

void testThePairingCodeSurvivesBeingTypedByAHuman() {
  uint8_t secret[vault::kSecretLen];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = static_cast<uint8_t>(i * 13 + 7);
  char text[33];
  vault::encodeSecret(secret, text, sizeof(text));
  check(std::strlen(text) == 32, "a 160-bit secret is exactly 32 symbols, with no padding");

  uint8_t back[vault::kSecretLen];
  check(vault::decodeSecret(text, back), "the code it printed is a code it accepts");
  check(std::memcmp(secret, back, sizeof(secret)) == 0, "and it round-trips to the same 20 bytes");

  // The form it is actually read off the screen in: eight groups of four.
  std::string grouped;
  for (int i = 0; i < 32; ++i) {
    if (i > 0 && i % 4 == 0) grouped.push_back('-');
    grouped.push_back(text[i]);
  }
  check(vault::decodeSecret(grouped.c_str(), back) && std::memcmp(secret, back, sizeof(secret)) == 0,
        "separators between the groups are ignored");

  std::string lower;
  for (int i = 0; i < 32; ++i) lower.push_back(static_cast<char>(std::tolower(text[i])));
  check(vault::decodeSecret(lower.c_str(), back) && std::memcmp(secret, back, sizeof(secret)) == 0,
        "lowercase is accepted");

  // The three folds the alphabet exists to allow.
  uint8_t zeros[vault::kSecretLen];
  uint8_t letters[vault::kSecretLen];
  check(vault::decodeSecret("00000000000000000000000000000000", zeros), "all-zero decodes");
  check(vault::decodeSecret("OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO", letters), "the letter O decodes");
  check(std::memcmp(zeros, letters, sizeof(zeros)) == 0, "and O folds to zero");
  check(vault::decodeSecret("11111111111111111111111111111111", zeros) &&
            vault::decodeSecret("IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII", letters) &&
            std::memcmp(zeros, letters, sizeof(zeros)) == 0,
        "I folds to one");
  check(vault::decodeSecret("LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL", letters) &&
            std::memcmp(zeros, letters, sizeof(zeros)) == 0,
        "L folds to one");

  check(!vault::decodeSecret("", back), "an empty code is refused");
  check(!vault::decodeSecret("0000000000000000000000000000000", back), "one symbol short is refused");
  check(!vault::decodeSecret("000000000000000000000000000000000", back), "one symbol long is refused");
  check(!vault::decodeSecret("0000000000000000000000000000000U", back), "a symbol outside the alphabet is refused");
}

// --- The PIN ----------------------------------------------------------------

void testTheSealedSecretGivesNothingAwayOffline() {
  uint8_t salt[vault::kSaltLen];
  for (size_t i = 0; i < sizeof(salt); ++i) salt[i] = static_cast<uint8_t>(i * 17 + 5);

  uint8_t sealed[vault::kSecretLen];
  vault::sealSecret(kSecret, "482913", salt, sealed);

  uint8_t opened[vault::kSecretLen];
  vault::openSecret(sealed, "482913", salt, opened);
  check(std::memcmp(opened, kSecret, sizeof(opened)) == 0, "the right PIN gives the secret back");

  check(std::memcmp(sealed, kSecret, sizeof(sealed)) != 0, "the sealed bytes are not the secret");

  // The claim the whole design rests on: a wrong PIN yields a DIFFERENT
  // well-formed secret, with nothing to mark it wrong. If openSecret ever
  // grew a tag or a checksum, this test would still pass -- so the check
  // that matters is the one below it.
  vault::openSecret(sealed, "482914", salt, opened);
  check(std::memcmp(opened, kSecret, sizeof(opened)) != 0, "a wrong PIN gives a wrong secret");

  // Every one of ten thousand four-digit PINs opens the blob into a secret
  // that is a valid pairing code. There is no local oracle, which is what
  // forces an attacker to ask the Mac -- and the Mac counts the asking.
  // Sampled rather than exhaustive: the stretch is deliberately expensive.
  char text[33];
  for (int guess = 0; guess < 2000; guess += 197) {
    char pin[8];
    std::snprintf(pin, sizeof(pin), "%04d", guess);
    vault::openSecret(sealed, pin, salt, opened);
    vault::encodeSecret(opened, text, sizeof(text));
    uint8_t back[vault::kSecretLen];
    check(std::strlen(text) == 32 && vault::decodeSecret(text, back),
          "a wrong PIN still produces a perfectly well-formed secret");
  }

  // A different salt is a different sealing, so two readers paired to the same
  // Mac with the same PIN do not share a blob.
  uint8_t otherSalt[vault::kSaltLen];
  std::memcpy(otherSalt, salt, sizeof(salt));
  otherSalt[0] = static_cast<uint8_t>(otherSalt[0] ^ 0x01);
  uint8_t otherSealed[vault::kSecretLen];
  vault::sealSecret(kSecret, "482913", otherSalt, otherSealed);
  check(std::memcmp(sealed, otherSealed, sizeof(sealed)) != 0, "the salt changes the sealing");

  check(!vault::pinIsWellFormed("123"), "three digits is too few");
  check(vault::pinIsWellFormed("1234"), "four digits is the floor");
  check(vault::pinIsWellFormed("123456789012"), "twelve digits is the ceiling");
  check(!vault::pinIsWellFormed("1234567890123"), "thirteen is too many");
  check(!vault::pinIsWellFormed("12a4"), "a letter is not a digit");
  check(!vault::pinIsWellFormed(""), "an empty PIN is not a PIN");
  check(!vault::pinIsWellFormed(nullptr), "no PIN at all is not a PIN");
}

// --- The frames -------------------------------------------------------------

void testTheFramesRoundTrip() {
  const vault::Challenge challenge = freshChallenge(vault::Op::Unlock, 42, 3);
  uint8_t wire[vault::kChallengeLen];
  vault::encodeChallenge(challenge, wire);

  vault::Challenge back;
  check(vault::decodeChallenge(wire, sizeof(wire), back), "a challenge decodes");
  check(back.version == challenge.version && back.op == challenge.op && back.counter == challenge.counter &&
            std::memcmp(back.nonce, challenge.nonce, vault::kNonceLen) == 0 &&
            std::memcmp(back.mac, challenge.mac, vault::kHashLen) == 0,
        "and every field survives the wire");

  check(!vault::decodeChallenge(wire, sizeof(wire) - 1, back), "a short challenge is refused");
  check(!vault::decodeChallenge(wire, sizeof(wire) + 1, back), "a long challenge is refused");
  uint8_t bent[vault::kChallengeLen];
  std::memcpy(bent, wire, sizeof(bent));
  bent[1] = 0x7f;
  check(!vault::decodeChallenge(bent, sizeof(bent), back), "an op we do not speak is refused at the parser");

  const char* password = "correct horse battery staple";
  vault::Response response;
  vault::buildResponse(kSecret, sizeof(kSecret), challenge, vault::Screen::Locked,
                       reinterpret_cast<const uint8_t*>(password), std::strlen(password), response);

  uint8_t out[vault::kResponseMaxLen];
  const size_t len = vault::encodeResponse(response, out, sizeof(out));
  check(len == vault::kResponseHeadLen + std::strlen(password) + vault::kHashLen, "the response is as long as it says");

  vault::Response decoded;
  check(vault::decodeResponse(out, len, decoded), "a response decodes");
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, decoded) == vault::Verdict::Ok,
        "and verifies against the challenge it answers");

  char typed[vault::kMaxSecretPayload + 1];
  const size_t typedLen = vault::openPayload(kSecret, sizeof(kSecret), challenge, decoded, typed, sizeof(typed));
  check(typedLen == std::strlen(password) && std::strcmp(typed, password) == 0,
        "and the password comes back out of it");
  check(std::memcmp(decoded.payload, password, std::strlen(password)) != 0,
        "while the bytes that crossed the wire were not the password");

  check(!vault::decodeResponse(out, len - 1, decoded), "a truncated response is refused");
  check(!vault::decodeResponse(out, vault::kResponseHeadLen + vault::kHashLen - 1, decoded),
        "a response shorter than its own head is refused");
  std::memcpy(bent, out, vault::kResponseHeadLen);
  bent[1] = 0x55;
  check(!vault::decodeResponse(bent, len, decoded), "a screen state we do not speak is refused at the parser");

  // A payload length field that lies about the frame.
  uint8_t lying[vault::kResponseMaxLen];
  std::memcpy(lying, out, len);
  lying[vault::kResponseHeadLen - 1] = static_cast<uint8_t>(vault::kMaxSecretPayload);
  check(!vault::decodeResponse(lying, len, decoded), "a length field that does not match the frame is refused");
}

// --- What an attacker gets to try -------------------------------------------

void testOnlyTheHolderOfTheSecretCanBeBelieved() {
  const vault::Challenge challenge = freshChallenge(vault::Op::Unlock, 7, 11);
  const char* password = "hunter2hunter2";

  vault::Response good;
  vault::buildResponse(kSecret, sizeof(kSecret), challenge, vault::Screen::Locked,
                       reinterpret_cast<const uint8_t*>(password), std::strlen(password), good);
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, good) == vault::Verdict::Ok, "the real answer");

  // A host that does not hold the secret -- which is every host but one.
  uint8_t wrong[vault::kSecretLen];
  std::memcpy(wrong, kSecret, sizeof(wrong));
  wrong[0] = static_cast<uint8_t>(wrong[0] ^ 0x01);
  vault::Response forged;
  vault::buildResponse(wrong, sizeof(wrong), challenge, vault::Screen::Locked,
                       reinterpret_cast<const uint8_t*>(password), std::strlen(password), forged);
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, forged) == vault::Verdict::BadMac,
        "a one-bit-different secret cannot answer");

  // The wrong PIN, end to end: it is the same attack, arriving from the other
  // side. This is the ONLY place a mistyped PIN can be noticed.
  uint8_t salt[vault::kSaltLen] = {};
  uint8_t sealed[vault::kSecretLen];
  vault::sealSecret(kSecret, "482913", salt, sealed);
  uint8_t mistyped[vault::kSecretLen];
  vault::openSecret(sealed, "482931", salt, mistyped);
  const vault::Challenge underMistyped = [&] {
    vault::Challenge c = challenge;
    vault::signChallenge(mistyped, sizeof(mistyped), c);
    return c;
  }();
  vault::Response answered;
  vault::buildResponse(kSecret, sizeof(kSecret), underMistyped, vault::Screen::Locked,
                       reinterpret_cast<const uint8_t*>(password), std::strlen(password), answered);
  check(vault::verifyResponse(mistyped, sizeof(mistyped), underMistyped, answered) == vault::Verdict::BadMac,
        "a mistyped PIN fails at the Mac and nowhere earlier");

  // Flipping the state the button reads. This is the attack that matters most
  // on an unlocked Mac: convince the reader the screen is locked and it types
  // the password into whatever has focus.
  vault::Response flipped = good;
  flipped.screen = vault::Screen::Unlocked;
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, flipped) == vault::Verdict::BadMac,
        "the screen state is inside the MAC, so it cannot be flipped");

  // Replay against a new nonce, which is what a recorded answer is.
  const vault::Challenge later = freshChallenge(vault::Op::Unlock, 8, 12);
  check(vault::verifyResponse(kSecret, sizeof(kSecret), later, good) == vault::Verdict::CounterMismatch,
        "an answer to an older challenge is not an answer to this one");

  vault::Response sameCounter = good;
  sameCounter.counter = later.counter;
  check(vault::verifyResponse(kSecret, sizeof(kSecret), later, sameCounter) == vault::Verdict::BadMac,
        "and correcting the counter does not make the MAC fit the new nonce");

  // A Status answer standing in for an Unlock one. The op is in the MAC
  // input but not in the response frame, so this is the test that proves it
  // is actually covered.
  vault::Challenge statusChallenge = challenge;
  statusChallenge.op = vault::Op::Status;
  vault::signChallenge(kSecret, sizeof(kSecret), statusChallenge);
  vault::Response statusAnswer;
  vault::buildResponse(kSecret, sizeof(kSecret), statusChallenge, vault::Screen::Locked, nullptr, 0, statusAnswer);
  statusAnswer.counter = challenge.counter;
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, statusAnswer) == vault::Verdict::BadMac,
        "an answer to a Status request cannot stand in for an answer to an Unlock one");

  vault::Response skewed = good;
  skewed.version = vault::kProtocolVersion + 1;
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, skewed) == vault::Verdict::BadVersion,
        "a frame from a protocol we do not speak is refused before its fields are read");

  vault::Response oversize = good;
  oversize.payloadLen = static_cast<uint8_t>(vault::kMaxSecretPayload + 1);
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, oversize) == vault::Verdict::BadPayload,
        "a payload longer than anything we would type is refused");

  // Every bit of the MAC.
  for (size_t i = 0; i < vault::kHashLen; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      vault::Response bent = good;
      bent.mac[i] = static_cast<uint8_t>(bent.mac[i] ^ (1 << bit));
      check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, bent) == vault::Verdict::BadMac,
            "a one-bit change to the MAC is caught");
    }
  }

  // Every bit of the ciphertext, which encrypt-then-MAC promises to catch
  // BEFORE anything tries to decrypt it.
  for (size_t i = 0; i < good.payloadLen; ++i) {
    vault::Response bent = good;
    bent.payload[i] = static_cast<uint8_t>(bent.payload[i] ^ 0x40);
    check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, bent) == vault::Verdict::BadMac,
          "a bent ciphertext byte is caught by the tag, not by the parser");
  }
}

void testTheChallengeIsAuthenticatedToo() {
  // Without this the Mac would hand its password to anyone who wrote 58 bytes
  // to the characteristic.
  vault::Challenge challenge = freshChallenge(vault::Op::Unlock, 99, 5);
  uint8_t key[vault::kHashLen];
  vault::deriveKey(kSecret, sizeof(kSecret), "crossplay/unlock/1/req", challenge.nonce, key);

  vault::Challenge check1 = challenge;
  vault::signChallenge(kSecret, sizeof(kSecret), check1);
  check(std::memcmp(check1.mac, challenge.mac, vault::kHashLen) == 0, "signing is deterministic");

  vault::Challenge tampered = challenge;
  tampered.op = vault::Op::Status;
  vault::Challenge resigned = tampered;
  vault::signChallenge(kSecret, sizeof(kSecret), resigned);
  check(std::memcmp(resigned.mac, tampered.mac, vault::kHashLen) != 0, "changing the op invalidates the request MAC");

  vault::Challenge movedCounter = challenge;
  movedCounter.counter += 1;
  resigned = movedCounter;
  vault::signChallenge(kSecret, sizeof(kSecret), resigned);
  check(std::memcmp(resigned.mac, movedCounter.mac, vault::kHashLen) != 0,
        "changing the counter invalidates the request MAC");

  // The three keys have to be three keys. If the label ever stopped reaching
  // the HMAC, a request MAC would double as a response MAC.
  uint8_t response[vault::kHashLen];
  uint8_t encrypt[vault::kHashLen];
  vault::deriveKey(kSecret, sizeof(kSecret), "crossplay/unlock/1/res", challenge.nonce, response);
  vault::deriveKey(kSecret, sizeof(kSecret), "crossplay/unlock/1/enc", challenge.nonce, encrypt);
  check(std::memcmp(key, response, sizeof(key)) != 0 && std::memcmp(key, encrypt, sizeof(key)) != 0 &&
            std::memcmp(response, encrypt, sizeof(key)) != 0,
        "the three purposes get three different keys");

  uint8_t otherNonce[vault::kHashLen];
  vault::Challenge second = freshChallenge(vault::Op::Unlock, 99, 6);
  vault::deriveKey(kSecret, sizeof(kSecret), "crossplay/unlock/1/enc", second.nonce, otherNonce);
  check(std::memcmp(encrypt, otherNonce, sizeof(encrypt)) != 0,
        "and a new nonce gives a new keystream, so two sends never share one");
}

void testTheButtonRefusesToGuess() {
  check(vault::intentFor(vault::Screen::Locked) == vault::Intent::Unlock, "a locked Mac is one to unlock");
  check(vault::intentFor(vault::Screen::Unlocked) == vault::Intent::Lock, "an unlocked Mac is one to lock");
  check(vault::intentFor(vault::Screen::Unknown) == vault::Intent::Ask,
        "and with nothing verified the button asks rather than typing");

  const vault::LockChord chord = vault::lockChord();
  check(chord.modifiers == (1 | 8) && chord.key == 0x14, "the lock chord is Control-Command-Q");
}

void testAnEmptyPayloadTypesNothing() {
  const vault::Challenge challenge = freshChallenge(vault::Op::Status, 1, 2);
  vault::Response status;
  vault::buildResponse(kSecret, sizeof(kSecret), challenge, vault::Screen::Unlocked, nullptr, 0, status);
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, status) == vault::Verdict::Ok,
        "a Status answer carries no password and still verifies");

  char typed[vault::kMaxSecretPayload + 1];
  std::memset(typed, 'x', sizeof(typed));
  check(vault::openPayload(kSecret, sizeof(kSecret), challenge, status, typed, sizeof(typed)) == 0,
        "and there is nothing to type");
  check(typed[0] == '\0', "the buffer is cleared rather than left holding whatever was in it");

  // A password exactly at the ceiling, which is the one length an off-by-one
  // in either direction gets wrong.
  uint8_t longest[vault::kMaxSecretPayload];
  for (size_t i = 0; i < sizeof(longest); ++i) longest[i] = static_cast<uint8_t>('a' + (i % 26));
  vault::Response full;
  vault::buildResponse(kSecret, sizeof(kSecret), challenge, vault::Screen::Locked, longest, sizeof(longest), full);
  check(vault::verifyResponse(kSecret, sizeof(kSecret), challenge, full) == vault::Verdict::Ok,
        "the longest password we accept verifies");
  const size_t typedLen = vault::openPayload(kSecret, sizeof(kSecret), challenge, full, typed, sizeof(typed));
  check(typedLen == sizeof(longest) && std::memcmp(typed, longest, sizeof(longest)) == 0 &&
            typed[sizeof(longest)] == '\0',
        "and comes back whole and terminated");

  // A buffer that cannot hold it must type nothing rather than a prefix: half
  // a password at a lock screen is a failed attempt the Mac counts.
  char tooSmall[16];
  check(vault::openPayload(kSecret, sizeof(kSecret), challenge, full, tooSmall, sizeof(tooSmall)) == 0 &&
            tooSmall[0] == '\0',
        "a buffer too small for the password yields nothing at all");
}

void testWipeActuallyWipes() {
  uint8_t buffer[32];
  for (size_t i = 0; i < sizeof(buffer); ++i) buffer[i] = static_cast<uint8_t>(i + 1);
  vault::wipe(buffer, sizeof(buffer));
  uint8_t any = 0;
  for (size_t i = 0; i < sizeof(buffer); ++i) any = static_cast<uint8_t>(any | buffer[i]);
  check(any == 0, "wipe leaves zeroes behind");
}

}  // namespace

int main() {
  testSha256AgainstPublishedVectors();
  testHmacAgainstRfc4231();
  testPbkdf2AgainstRfc7914();
  testConstantTimeCompareStillCompares();
  testThePairingCodeSurvivesBeingTypedByAHuman();
  testTheSealedSecretGivesNothingAwayOffline();
  testTheFramesRoundTrip();
  testOnlyTheHolderOfTheSecretCanBeBelieved();
  testTheChallengeIsAuthenticatedToo();
  testTheButtonRefusesToGuess();
  testAnEmptyPayloadTypesNothing();
  testWipeActuallyWipes();

  std::printf("%s  remote vault: %d checks, %d failed\n", failures == 0 ? "ok  " : "FAIL", checks, failures);
  return failures == 0 ? 0 : 1;
}
