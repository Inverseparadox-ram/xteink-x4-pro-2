#include "RemoteLink.h"

#include <Logging.h>

#if defined(CROSSPLAY_BLE_HID)
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#endif

#include <cstring>

namespace remote {
namespace helper {
namespace {

constexpr const char* kTag = "REMOTE";

// Random 128-bit UUIDs, so they collide with nothing and mean nothing. The
// Swift helper in docs/apps/remote.md has to match these three exactly.
constexpr const char* kServiceUuid = "6f1b0a00-9d3c-4f5e-8a77-2b4c1d6e9f01";
constexpr const char* kChallengeUuid = "6f1b0a01-9d3c-4f5e-8a77-2b4c1d6e9f01";
constexpr const char* kResponseUuid = "6f1b0a02-9d3c-4f5e-8a77-2b4c1d6e9f01";

#if defined(CROSSPLAY_BLE_HID)

NimBLEService* service = nullptr;
NimBLECharacteristic* challengeOut = nullptr;
NimBLECharacteristic* responseIn = nullptr;

// Written by the NimBLE host task, read by the activity task. One producer,
// one consumer, and `answerReady` is set last and cleared first -- so the
// consumer never sees a length that belongs to a frame still being copied.
volatile bool subscribed = false;
volatile bool answerReady = false;
uint8_t answer[vault::kResponseMaxLen];
volatile size_t answerLen = 0;

State state = State::Idle;
uint32_t askedAt = 0;

class ChallengeCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, const uint16_t value) override {
    subscribed = value != 0;
    LOG_INF(kTag, "unlock helper %s", subscribed ? "subscribed" : "gone");
  }
};

class ResponseCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue& value = characteristic->getValue();
    if (value.size() == 0 || value.size() > sizeof(answer)) {
      LOG_INF(kTag, "unlock: answer of %u bytes ignored", static_cast<unsigned>(value.size()));
      return;
    }
    std::memcpy(answer, value.data(), value.size());
    answerLen = value.size();
    answerReady = true;
  }
};

ChallengeCallbacks challengeCallbacks;
ResponseCallbacks responseCallbacks;

#endif  // CROSSPLAY_BLE_HID

}  // namespace

#if defined(CROSSPLAY_BLE_HID)

void begin() {
  if (service != nullptr) return;
  NimBLEServer* server = NimBLEDevice::getServer();
  if (server == nullptr) {
    LOG_ERR(kTag, "unlock: no BLE server; call remote::begin() first");
    return;
  }
  service = server->createService(kServiceUuid);
  if (service == nullptr) {
    LOG_ERR(kTag, "unlock: could not create the service");
    return;
  }
  // READ as well as NOTIFY: a helper that connects between two taps can read
  // the standing challenge rather than waiting for the next one.
  challengeOut = service->createCharacteristic(kChallengeUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  // WRITE, not WRITE_NR: the answer carries the password, and a write without
  // a response is one the Mac cannot tell arrived.
  responseIn = service->createCharacteristic(kResponseUuid, NIMBLE_PROPERTY::WRITE);
  if (challengeOut == nullptr || responseIn == nullptr) {
    LOG_ERR(kTag, "unlock: could not create the characteristics");
    return;
  }
  challengeOut->setCallbacks(&challengeCallbacks);
  responseIn->setCallbacks(&responseCallbacks);
  service->start();

  state = State::Idle;
  subscribed = false;
  answerReady = false;
  LOG_INF(kTag, "unlock service up");
}

void end() {
  service = nullptr;
  challengeOut = nullptr;
  responseIn = nullptr;
  subscribed = false;
  answerReady = false;
  answerLen = 0;
  state = State::Idle;
  vault::wipe(answer, sizeof(answer));
}

const char* serviceUuid() { return kServiceUuid; }

bool helperPresent() { return subscribed; }

bool ask(const vault::Challenge& challenge) {
  if (challengeOut == nullptr || !subscribed) return false;
  answerReady = false;
  answerLen = 0;
  uint8_t wire[vault::kChallengeLen];
  vault::encodeChallenge(challenge, wire);
  challengeOut->setValue(wire, sizeof(wire));
  challengeOut->notify();
  askedAt = millis();
  state = State::Waiting;
  return true;
}

State poll() {
  if (state == State::Waiting) {
    if (answerReady) {
      state = State::Answered;
    } else if (millis() - askedAt > kAnswerTimeoutMs) {
      state = State::TimedOut;
    }
  }
  return state;
}

bool take(vault::Response& out) {
  if (state != State::Answered || !answerReady) return false;
  const bool ok = vault::decodeResponse(answer, answerLen, out);
  answerReady = false;
  answerLen = 0;
  vault::wipe(answer, sizeof(answer));
  state = State::Idle;
  if (!ok) LOG_INF(kTag, "unlock: an answer arrived that is not a frame");
  return ok;
}

void cancel() {
  answerReady = false;
  answerLen = 0;
  state = State::Idle;
}

void randomBytes(uint8_t* out, const size_t len) {
  for (size_t at = 0; at < len; at += 4) {
    const uint32_t word = esp_random();
    const size_t take = (len - at) < 4 ? (len - at) : 4;
    std::memcpy(out + at, &word, take);
  }
}

#else  // simulator: no radio, so the screens can still be driven and rendered.

void begin() {}
void end() {}
const char* serviceUuid() { return kServiceUuid; }
bool helperPresent() { return false; }
bool ask(const vault::Challenge&) { return false; }
State poll() { return State::Idle; }
bool take(vault::Response&) { return false; }
void cancel() {}

void randomBytes(uint8_t* out, const size_t len) {
  // Never used to seal anything here -- the simulator has no host to answer --
  // but the buffer must not be left holding whatever was on the stack.
  static uint32_t counter = 0x12345678;
  for (size_t i = 0; i < len; ++i) {
    counter = counter * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(counter >> 24);
  }
}

#endif

}  // namespace helper
}  // namespace remote
