#include "RemoteVault.h"

#include <cstring>

namespace remote {
namespace vault {
namespace {

// --- SHA-256, FIPS 180-4 ----------------------------------------------------

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(const uint32_t x, const int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
  uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint64_t length = 0;  // bytes fed in
  uint8_t block[64] = {};
  size_t fill = 0;

  void compress(const uint8_t* data) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = static_cast<uint32_t>(data[i * 4]) << 24 | static_cast<uint32_t>(data[i * 4 + 1]) << 16 |
             static_cast<uint32_t>(data[i * 4 + 2]) << 8 | static_cast<uint32_t>(data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  void update(const uint8_t* data, size_t len) {
    length += len;
    while (len > 0) {
      const size_t take = (64 - fill) < len ? (64 - fill) : len;
      std::memcpy(block + fill, data, take);
      fill += take;
      data += take;
      len -= take;
      if (fill == 64) {
        compress(block);
        fill = 0;
      }
    }
  }

  void finish(uint8_t out[kHashLen]) {
    const uint64_t bits = length * 8;
    const uint8_t one = 0x80;
    update(&one, 1);
    const uint8_t zero = 0x00;
    while (fill != 56) update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    // Fed through update() so the padding path stays one code path; `length`
    // is no longer read after this point.
    update(tail, 8);
    for (int i = 0; i < 8; ++i) {
      out[i * 4] = static_cast<uint8_t>(state[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
  }
};

void putU64(uint8_t* out, const uint64_t value) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

uint64_t getU64(const uint8_t* in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | in[i];
  return value;
}

bool knownOp(const uint8_t raw) {
  return raw == static_cast<uint8_t>(Op::Status) || raw == static_cast<uint8_t>(Op::Unlock);
}

bool knownScreen(const uint8_t raw) {
  return raw == static_cast<uint8_t>(Screen::Unknown) || raw == static_cast<uint8_t>(Screen::Unlocked) ||
         raw == static_cast<uint8_t>(Screen::Locked);
}

// --- Crockford base32 -------------------------------------------------------

constexpr const char* kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int symbolValue(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  if (c >= '0' && c <= '9') return c - '0';
  // The three folds people actually need. O for zero, I and L for one: they
  // are why the alphabet leaves those letters out in the first place.
  if (c == 'O') return 0;
  if (c == 'I' || c == 'L') return 1;
  for (int i = 10; i < 32; ++i) {
    if (kAlphabet[i] == c) return i;
  }
  return -1;
}

// --- The labels that keep the three keys apart ------------------------------

constexpr const char* kLabelRequest = "crossplay/unlock/1/req";
constexpr const char* kLabelResponse = "crossplay/unlock/1/res";
constexpr const char* kLabelEncrypt = "crossplay/unlock/1/enc";

// Everything both sides MAC, for either direction. The response's bytes are a
// superset of the challenge's, so one builder serves both and there is no way
// for the two sides to disagree about the layout.
size_t macInput(const Challenge& challenge, const bool withResponse, const Screen screen, const uint8_t* payload,
                const size_t payloadLen, uint8_t* out, const size_t size) {
  const size_t need = 1 + 1 + 8 + kNonceLen + (withResponse ? 1 + 1 + payloadLen : 0);
  if (size < need) return 0;
  size_t at = 0;
  out[at++] = challenge.version;
  out[at++] = static_cast<uint8_t>(challenge.op);
  putU64(out + at, challenge.counter);
  at += 8;
  std::memcpy(out + at, challenge.nonce, kNonceLen);
  at += kNonceLen;
  if (withResponse) {
    out[at++] = static_cast<uint8_t>(screen);
    out[at++] = static_cast<uint8_t>(payloadLen);
    std::memcpy(out + at, payload, payloadLen);
    at += payloadLen;
  }
  return at;
}

constexpr size_t kMacInputMax = 1 + 1 + 8 + kNonceLen + 1 + 1 + kMaxSecretPayload;

// HMAC-SHA256 in counter mode. A stream cipher built from the one primitive
// this file already proves against published vectors, rather than a second
// primitive nobody here can test -- and the payload is one password, so the
// throughput a real cipher would buy is worth nothing.
void keystream(const uint8_t key[kHashLen], uint8_t* out, const size_t len) {
  uint8_t counter[4];
  uint8_t block[kHashLen];
  for (size_t at = 0; at < len; at += kHashLen) {
    const uint32_t index = static_cast<uint32_t>(at / kHashLen);
    counter[0] = static_cast<uint8_t>(index >> 24);
    counter[1] = static_cast<uint8_t>(index >> 16);
    counter[2] = static_cast<uint8_t>(index >> 8);
    counter[3] = static_cast<uint8_t>(index);
    hmacSha256(key, kHashLen, counter, sizeof(counter), block);
    const size_t take = (len - at) < kHashLen ? (len - at) : kHashLen;
    std::memcpy(out + at, block, take);
  }
  wipe(block, sizeof(block));
}

}  // namespace

// --- Primitives -------------------------------------------------------------

void sha256(const uint8_t* data, const size_t len, uint8_t out[kHashLen]) {
  Sha256 hash;
  hash.update(data, len);
  hash.finish(out);
}

void hmacSha256(const uint8_t* key, const size_t keyLen, const uint8_t* data, const size_t len, uint8_t out[kHashLen]) {
  uint8_t padded[64] = {};
  if (keyLen > 64) {
    sha256(key, keyLen, padded);
  } else {
    std::memcpy(padded, key, keyLen);
  }
  uint8_t inner[64];
  uint8_t outer[64];
  for (int i = 0; i < 64; ++i) {
    inner[i] = static_cast<uint8_t>(padded[i] ^ 0x36);
    outer[i] = static_cast<uint8_t>(padded[i] ^ 0x5c);
  }
  uint8_t innerHash[kHashLen];
  Sha256 first;
  first.update(inner, sizeof(inner));
  first.update(data, len);
  first.finish(innerHash);

  Sha256 second;
  second.update(outer, sizeof(outer));
  second.update(innerHash, sizeof(innerHash));
  second.finish(out);

  wipe(padded, sizeof(padded));
  wipe(inner, sizeof(inner));
  wipe(outer, sizeof(outer));
}

bool constantTimeEquals(const uint8_t* a, const uint8_t* b, const size_t len) {
  uint8_t difference = 0;
  for (size_t i = 0; i < len; ++i) difference = static_cast<uint8_t>(difference | (a[i] ^ b[i]));
  return difference == 0;
}

void pbkdf2(const uint8_t* password, const size_t passwordLen, const uint8_t* salt, const size_t saltLen,
            const uint32_t iterations, uint8_t* out, const size_t outLen) {
  uint8_t block[kHashLen];
  uint8_t accumulator[kHashLen];
  // The salt with the big-endian block index appended, which is what PBKDF2's
  // first iteration hashes.
  uint8_t seed[kSaltLen + 64 + 4];
  const size_t seedSaltLen = saltLen > sizeof(seed) - 4 ? sizeof(seed) - 4 : saltLen;
  std::memcpy(seed, salt, seedSaltLen);

  for (size_t done = 0; done < outLen;) {
    const uint32_t index = static_cast<uint32_t>(done / kHashLen) + 1;
    seed[seedSaltLen] = static_cast<uint8_t>(index >> 24);
    seed[seedSaltLen + 1] = static_cast<uint8_t>(index >> 16);
    seed[seedSaltLen + 2] = static_cast<uint8_t>(index >> 8);
    seed[seedSaltLen + 3] = static_cast<uint8_t>(index);
    hmacSha256(password, passwordLen, seed, seedSaltLen + 4, block);
    std::memcpy(accumulator, block, kHashLen);
    for (uint32_t i = 1; i < iterations; ++i) {
      hmacSha256(password, passwordLen, block, kHashLen, block);
      for (size_t j = 0; j < kHashLen; ++j) accumulator[j] = static_cast<uint8_t>(accumulator[j] ^ block[j]);
    }
    const size_t take = (outLen - done) < kHashLen ? (outLen - done) : kHashLen;
    std::memcpy(out + done, accumulator, take);
    done += take;
  }
  wipe(block, sizeof(block));
  wipe(accumulator, sizeof(accumulator));
}

void wipe(void* data, const size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) p[i] = 0;
}

// --- The shared secret ------------------------------------------------------

void encodeSecret(const uint8_t secret[kSecretLen], char* out, const size_t size) {
  if (size < 33) {
    if (size > 0) out[0] = '\0';
    return;
  }
  // 20 bytes is 160 bits, which is exactly 32 five-bit symbols: no padding and
  // no partial final symbol to get wrong.
  for (size_t i = 0; i < 32; ++i) {
    const size_t bit = i * 5;
    const size_t byte = bit / 8;
    const int shift = static_cast<int>(bit % 8);
    uint32_t window = static_cast<uint32_t>(secret[byte]) << 8;
    if (byte + 1 < kSecretLen) window |= secret[byte + 1];
    out[i] = kAlphabet[(window >> (11 - shift)) & 0x1f];
  }
  out[32] = '\0';
}

bool decodeSecret(const char* text, uint8_t out[kSecretLen]) {
  uint8_t symbols[32];
  size_t count = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    const char c = *p;
    if (c == '-' || c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
    if (count >= 32) return false;
    const int value = symbolValue(c);
    if (value < 0) return false;
    symbols[count++] = static_cast<uint8_t>(value);
  }
  if (count != 32) return false;
  std::memset(out, 0, kSecretLen);
  for (size_t i = 0; i < 32; ++i) {
    const size_t bit = i * 5;
    const size_t byte = bit / 8;
    const int shift = static_cast<int>(bit % 8);
    const uint32_t window = static_cast<uint32_t>(symbols[i]) << (11 - shift);
    out[byte] = static_cast<uint8_t>(out[byte] | (window >> 8));
    if (byte + 1 < kSecretLen) out[byte + 1] = static_cast<uint8_t>(out[byte + 1] | (window & 0xff));
  }
  return true;
}

// --- Sealing ----------------------------------------------------------------

void sealSecret(const uint8_t secret[kSecretLen], const char* pin, const uint8_t salt[kSaltLen],
                uint8_t out[kSecretLen]) {
  uint8_t key[kSecretLen];
  pbkdf2(reinterpret_cast<const uint8_t*>(pin), std::strlen(pin), salt, kSaltLen, kPinIterations, key, sizeof(key));
  for (size_t i = 0; i < kSecretLen; ++i) out[i] = static_cast<uint8_t>(secret[i] ^ key[i]);
  wipe(key, sizeof(key));
}

void openSecret(const uint8_t sealed[kSecretLen], const char* pin, const uint8_t salt[kSaltLen],
                uint8_t out[kSecretLen]) {
  // XOR is its own inverse, which is the only reason one function would do.
  sealSecret(sealed, pin, salt, out);
}

bool pinIsWellFormed(const char* pin) {
  if (pin == nullptr) return false;
  size_t len = 0;
  for (const char* p = pin; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    ++len;
  }
  return len >= kPinMinLen && len <= kPinMaxLen;
}

// --- The exchange -----------------------------------------------------------

void deriveKey(const uint8_t* secret, const size_t secretLen, const char* label, const uint8_t nonce[kNonceLen],
               uint8_t out[kHashLen]) {
  uint8_t input[64 + kNonceLen];
  size_t labelLen = std::strlen(label);
  if (labelLen > 64) labelLen = 64;
  std::memcpy(input, label, labelLen);
  std::memcpy(input + labelLen, nonce, kNonceLen);
  hmacSha256(secret, secretLen, input, labelLen + kNonceLen, out);
}

void signChallenge(const uint8_t* secret, const size_t secretLen, Challenge& challenge) {
  uint8_t key[kHashLen];
  deriveKey(secret, secretLen, kLabelRequest, challenge.nonce, key);
  uint8_t input[kMacInputMax];
  const size_t len = macInput(challenge, false, Screen::Unknown, nullptr, 0, input, sizeof(input));
  hmacSha256(key, sizeof(key), input, len, challenge.mac);
  wipe(key, sizeof(key));
}

void encodeChallenge(const Challenge& in, uint8_t out[kChallengeLen]) {
  size_t at = 0;
  out[at++] = in.version;
  out[at++] = static_cast<uint8_t>(in.op);
  putU64(out + at, in.counter);
  at += 8;
  std::memcpy(out + at, in.nonce, kNonceLen);
  at += kNonceLen;
  std::memcpy(out + at, in.mac, kHashLen);
}

bool decodeChallenge(const uint8_t* in, const size_t len, Challenge& out) {
  if (len != kChallengeLen) return false;
  if (!knownOp(in[1])) return false;
  size_t at = 0;
  out.version = in[at++];
  out.op = static_cast<Op>(in[at++]);
  out.counter = getU64(in + at);
  at += 8;
  std::memcpy(out.nonce, in + at, kNonceLen);
  at += kNonceLen;
  std::memcpy(out.mac, in + at, kHashLen);
  return true;
}

void buildResponse(const uint8_t* secret, const size_t secretLen, const Challenge& challenge, const Screen screen,
                   const uint8_t* payload, size_t payloadLen, Response& out) {
  if (payloadLen > kMaxSecretPayload) payloadLen = kMaxSecretPayload;
  out.version = kProtocolVersion;
  out.screen = screen;
  out.counter = challenge.counter;
  out.payloadLen = static_cast<uint8_t>(payloadLen);
  std::memset(out.payload, 0, sizeof(out.payload));

  if (payloadLen > 0) {
    uint8_t encKey[kHashLen];
    deriveKey(secret, secretLen, kLabelEncrypt, challenge.nonce, encKey);
    uint8_t stream[kMaxSecretPayload];
    keystream(encKey, stream, payloadLen);
    for (size_t i = 0; i < payloadLen; ++i) out.payload[i] = static_cast<uint8_t>(payload[i] ^ stream[i]);
    wipe(stream, sizeof(stream));
    wipe(encKey, sizeof(encKey));
  }

  // Encrypt-then-MAC: the tag covers the ciphertext, so a bent payload is
  // rejected before anything tries to decrypt it.
  uint8_t macKey[kHashLen];
  deriveKey(secret, secretLen, kLabelResponse, challenge.nonce, macKey);
  uint8_t input[kMacInputMax];
  const size_t len = macInput(challenge, true, screen, out.payload, payloadLen, input, sizeof(input));
  hmacSha256(macKey, sizeof(macKey), input, len, out.mac);
  wipe(macKey, sizeof(macKey));
}

size_t encodeResponse(const Response& in, uint8_t* out, const size_t size) {
  if (in.payloadLen > kMaxSecretPayload) return 0;
  const size_t need = kResponseHeadLen + in.payloadLen + kHashLen;
  if (size < need) return 0;
  size_t at = 0;
  out[at++] = in.version;
  out[at++] = static_cast<uint8_t>(in.screen);
  putU64(out + at, in.counter);
  at += 8;
  out[at++] = in.payloadLen;
  std::memcpy(out + at, in.payload, in.payloadLen);
  at += in.payloadLen;
  std::memcpy(out + at, in.mac, kHashLen);
  return need;
}

bool decodeResponse(const uint8_t* in, const size_t len, Response& out) {
  if (len < kResponseHeadLen + kHashLen) return false;
  if (!knownScreen(in[1])) return false;
  const uint8_t payloadLen = in[kResponseHeadLen - 1];
  if (payloadLen > kMaxSecretPayload) return false;
  if (len != kResponseHeadLen + payloadLen + kHashLen) return false;
  size_t at = 0;
  out.version = in[at++];
  out.screen = static_cast<Screen>(in[at++]);
  out.counter = getU64(in + at);
  at += 8;
  out.payloadLen = in[at++];
  std::memset(out.payload, 0, sizeof(out.payload));
  std::memcpy(out.payload, in + at, payloadLen);
  at += payloadLen;
  std::memcpy(out.mac, in + at, kHashLen);
  return true;
}

Verdict verifyResponse(const uint8_t* secret, const size_t secretLen, const Challenge& challenge,
                       const Response& response) {
  // Version first, because a frame from a protocol we do not speak has no
  // fields we are entitled to read.
  if (response.version != kProtocolVersion) return Verdict::BadVersion;
  if (response.counter != challenge.counter) return Verdict::CounterMismatch;
  if (response.payloadLen > kMaxSecretPayload) return Verdict::BadPayload;

  uint8_t macKey[kHashLen];
  deriveKey(secret, secretLen, kLabelResponse, challenge.nonce, macKey);
  uint8_t input[kMacInputMax];
  const size_t len =
      macInput(challenge, true, response.screen, response.payload, response.payloadLen, input, sizeof(input));
  uint8_t expected[kHashLen];
  hmacSha256(macKey, sizeof(macKey), input, len, expected);
  wipe(macKey, sizeof(macKey));
  const bool ok = constantTimeEquals(expected, response.mac, kHashLen);
  wipe(expected, sizeof(expected));
  return ok ? Verdict::Ok : Verdict::BadMac;
}

size_t openPayload(const uint8_t* secret, const size_t secretLen, const Challenge& challenge, const Response& response,
                   char* out, const size_t size) {
  if (size == 0) return 0;
  out[0] = '\0';
  if (response.payloadLen == 0 || response.payloadLen > kMaxSecretPayload) return 0;
  if (static_cast<size_t>(response.payloadLen) + 1 > size) return 0;

  uint8_t encKey[kHashLen];
  deriveKey(secret, secretLen, kLabelEncrypt, challenge.nonce, encKey);
  uint8_t stream[kMaxSecretPayload];
  keystream(encKey, stream, response.payloadLen);
  for (size_t i = 0; i < response.payloadLen; ++i) {
    out[i] = static_cast<char>(response.payload[i] ^ stream[i]);
  }
  out[response.payloadLen] = '\0';
  wipe(stream, sizeof(stream));
  wipe(encKey, sizeof(encKey));
  return response.payloadLen;
}

// --- What the button does ---------------------------------------------------

Intent intentFor(const Screen screen) {
  switch (screen) {
    case Screen::Locked:
      return Intent::Unlock;
    case Screen::Unlocked:
      return Intent::Lock;
    case Screen::Unknown:
    default:
      return Intent::Ask;
  }
}

LockChord lockChord() {
  // Control(1) + Command(8), Q = HID usage 0x14.
  LockChord chord{};
  chord.modifiers = 1 | 8;
  chord.key = 0x14;
  return chord;
}

}  // namespace vault
}  // namespace remote
