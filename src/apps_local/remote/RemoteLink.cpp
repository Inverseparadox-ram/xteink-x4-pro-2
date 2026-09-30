#include "RemoteLink.h"

#include <Logging.h>

#if defined(CROSSPLAY_BLE_HID)
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#endif

#include <cstdio>
#include <cstdlib>
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
constexpr const char* kNowPlayingUuid = "6f1b0a03-9d3c-4f5e-8a77-2b4c1d6e9f01";
constexpr const char* kCommandUuid = "6f1b0a04-9d3c-4f5e-8a77-2b4c1d6e9f01";
constexpr const char* kMacStateUuid = "6f1b0a05-9d3c-4f5e-8a77-2b4c1d6e9f01";
constexpr const char* kStatusUuid = "6f1b0a06-9d3c-4f5e-8a77-2b4c1d6e9f01";

// Owned by the activity task: takeStatus() feeds it and statusBoards() reads it.
StatusAssembler statusShown;

#if defined(CROSSPLAY_BLE_HID)

NimBLEService* service = nullptr;
NimBLECharacteristic* challengeOut = nullptr;
NimBLECharacteristic* responseIn = nullptr;
NimBLECharacteristic* nowPlayingIn = nullptr;
NimBLECharacteristic* commandOut = nullptr;
NimBLECharacteristic* macStateIn = nullptr;
NimBLECharacteristic* statusIn = nullptr;
volatile bool commandSubscribed = false;

// Written by the NimBLE host task, read by the activity task. One producer,
// one consumer, and `answerReady` is set last and cleared first -- so the
// consumer never sees a length that belongs to a frame still being copied.
volatile bool subscribed = false;
volatile bool answerReady = false;
uint8_t answer[vault::kResponseMaxLen];
volatile size_t answerLen = 0;

State state = State::Idle;
uint32_t askedAt = 0;

// Unlike the answer, now-playing frames arrive whenever the Mac likes -- two
// track changes can land while the activity is mid-copy -- so this one is
// guarded properly rather than by flag ordering. The spinlock is right across
// the S3's two cores and costs a 132-byte memcpy.
portMUX_TYPE nowLock = portMUX_INITIALIZER_UNLOCKED;
uint8_t nowFrame[kNowPlayingFrameMax];
size_t nowLen = 0;
bool nowPending = false;
NowPlaying nowShown;

void queueNowPlaying(const uint8_t* data, const size_t len) {
  if (len > sizeof(nowFrame)) return;
  taskENTER_CRITICAL(&nowLock);
  std::memcpy(nowFrame, data, len);
  nowLen = len;
  nowPending = true;
  taskEXIT_CRITICAL(&nowLock);
}

// A frame meaning "nothing is playing", queued when the helper goes away so
// the panel does not keep a song from a Mac it can no longer hear.
void queueNothingPlaying() {
  const uint8_t nothing[] = {kNowPlayingVersion, static_cast<uint8_t>(NowPlayingState::Nothing), 0, 0};
  queueNowPlaying(nothing, sizeof(nothing));
}

// The Mac's state, under the same lock. `macStateGone` asks the activity to
// forget what it knew, which a decoded frame cannot express: "not known" is
// the absence of a frame, not a value one carries.
uint8_t macStateFrame[kMacStateLen];
bool macStatePending = false;
bool macStateGone = false;
MacState macStateShown;

void queueMacState(const uint8_t* data, const size_t len) {
  if (len != sizeof(macStateFrame)) return;
  taskENTER_CRITICAL(&nowLock);
  std::memcpy(macStateFrame, data, len);
  macStatePending = true;
  macStateGone = false;
  taskEXIT_CRITICAL(&nowLock);
}

void forgetMacState() {
  taskENTER_CRITICAL(&nowLock);
  macStatePending = false;
  macStateGone = true;
  taskEXIT_CRITICAL(&nowLock);
}

// Status rows, queued by the NimBLE host task and drained by the activity. A
// board is a burst of up to kStatusRowsMax writes, two boards can land back to
// back, and a row dropped on overflow only abandons that one board.
constexpr size_t kStatusQueue = 2 * kStatusRowsMax + 4;
struct QueuedRow {
  uint8_t len;
  uint8_t data[kStatusFrameMax];
};
QueuedRow statusQueue[kStatusQueue];
size_t statusHead = 0;  // next to read
size_t statusCount = 0;
bool statusGone = false;

void queueStatus(const uint8_t* data, const size_t len) {
  if (len == 0 || len > kStatusFrameMax) return;
  taskENTER_CRITICAL(&nowLock);
  if (statusCount < kStatusQueue) {
    QueuedRow& slot = statusQueue[(statusHead + statusCount) % kStatusQueue];
    slot.len = static_cast<uint8_t>(len);
    std::memcpy(slot.data, data, len);
    ++statusCount;
  }
  taskEXIT_CRITICAL(&nowLock);
}

void forgetStatus() {
  taskENTER_CRITICAL(&nowLock);
  statusCount = 0;
  statusGone = true;
  taskEXIT_CRITICAL(&nowLock);
}

class ChallengeCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, const uint16_t value) override {
    subscribed = value != 0;
    LOG_INF(kTag, "unlock helper %s", subscribed ? "subscribed" : "gone");
    if (!subscribed) {
      queueNothingPlaying();
      forgetMacState();
      forgetStatus();
    }
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, const uint16_t value) override {
    commandSubscribed = value != 0;
  }
};

class StatusCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue& value = characteristic->getValue();
    queueStatus(value.data(), value.size());
  }
};

class MacStateCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue& value = characteristic->getValue();
    queueMacState(value.data(), value.size());
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

class NowPlayingCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue& value = characteristic->getValue();
    queueNowPlaying(value.data(), value.size());
  }
};

ChallengeCallbacks challengeCallbacks;
ResponseCallbacks responseCallbacks;
NowPlayingCallbacks nowPlayingCallbacks;
CommandCallbacks commandCallbacks;
MacStateCallbacks macStateCallbacks;
StatusCallbacks statusCallbacks;

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
  // WRITE_ENC: an encrypted link, which here means the Mac that bonded as a
  // keyboard. Not a secret -- it is a song title -- but it is text on this
  // screen, and a stranger in Bluetooth range should not get to write any.
  // CoreBluetooth answers the encryption requirement on its own by encrypting
  // the link it already has.
  nowPlayingIn = service->createCharacteristic(kNowPlayingUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  commandOut = service->createCharacteristic(kCommandUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  // Encrypted for the same reason as now playing: it is the Mac's word about
  // its own microphones, and only the bonded Mac should be able to say it.
  macStateIn = service->createCharacteristic(kMacStateUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  // Pages 2 and 3, encrypted for the same reason: session titles are the
  // Mac's business, and only the bonded Mac should be able to write them.
  statusIn =
      service->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, kStatusFrameMax);
  if (challengeOut == nullptr || responseIn == nullptr || nowPlayingIn == nullptr || commandOut == nullptr ||
      macStateIn == nullptr || statusIn == nullptr) {
    LOG_ERR(kTag, "unlock: could not create the characteristics");
    return;
  }
  commandOut->setCallbacks(&commandCallbacks);
  macStateIn->setCallbacks(&macStateCallbacks);
  statusIn->setCallbacks(&statusCallbacks);
  challengeOut->setCallbacks(&challengeCallbacks);
  responseIn->setCallbacks(&responseCallbacks);
  nowPlayingIn->setCallbacks(&nowPlayingCallbacks);
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
  nowPlayingIn = nullptr;
  commandOut = nullptr;
  macStateIn = nullptr;
  statusIn = nullptr;
  commandSubscribed = false;
  queueNothingPlaying();
  forgetMacState();
  forgetStatus();
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

bool takeNowPlaying(NowPlaying& out) {
  uint8_t frame[kNowPlayingFrameMax];
  size_t len = 0;
  taskENTER_CRITICAL(&nowLock);
  const bool pending = nowPending;
  if (pending) {
    std::memcpy(frame, nowFrame, nowLen);
    len = nowLen;
    nowPending = false;
  }
  taskEXIT_CRITICAL(&nowLock);
  if (!pending) return false;

  // Decoded outside the lock: the parse is the slow part and needs no
  // protection, since `frame` is ours now.
  NowPlaying next = nowShown;
  if (!decodeNowPlaying(frame, len, next)) {
    LOG_INF(kTag, "now playing: a frame of %u bytes did not parse", static_cast<unsigned>(len));
    return false;
  }
  if (sameNowPlaying(next, nowShown)) return false;
  nowShown = next;
  out = nowShown;
  return true;
}

bool sendCommand(const MacCommand command) {
  if (commandOut == nullptr || !commandSubscribed) return false;
  uint8_t frame[kCommandLen];
  encodeCommand(command, frame);
  commandOut->setValue(frame, sizeof(frame));
  commandOut->notify();
  return true;
}

bool takeMacState(MacState& out) {
  uint8_t frame[kMacStateLen];
  taskENTER_CRITICAL(&nowLock);
  const bool pending = macStatePending;
  const bool gone = macStateGone;
  if (pending) std::memcpy(frame, macStateFrame, sizeof(frame));
  macStatePending = false;
  macStateGone = false;
  taskEXIT_CRITICAL(&nowLock);

  MacState next = macStateShown;
  if (gone) {
    next = MacState{};
  } else if (!pending || !decodeMacState(frame, sizeof(frame), next)) {
    return false;
  }
  if (next.known == macStateShown.known && next.microphonesMuted == macStateShown.microphonesMuted) return false;
  macStateShown = next;
  out = macStateShown;
  return true;
}

bool takeStatus() {
  bool changed = false;
  taskENTER_CRITICAL(&nowLock);
  const bool gone = statusGone;
  statusGone = false;
  taskEXIT_CRITICAL(&nowLock);
  if (gone) {
    statusShown.forget();
    changed = true;
  }
  // One row per lock: feed() is the slow part and needs no lock of its own.
  for (;;) {
    QueuedRow row;
    taskENTER_CRITICAL(&nowLock);
    const bool have = statusCount > 0;
    if (have) {
      row = statusQueue[statusHead];
      statusHead = (statusHead + 1) % kStatusQueue;
      --statusCount;
    }
    taskEXIT_CRITICAL(&nowLock);
    if (!have) break;
    if (statusShown.feed(row.data, row.len)) changed = true;
  }
  return changed;
}

const StatusAssembler& statusBoards() { return statusShown; }

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
// CROSSPOINT_SIM_HELPER=1 lets the padlock get as far as asking the Mac, in a
// simulator that has no radio and so no helper to ask.
bool helperPresent() {
  const char* env = std::getenv("CROSSPOINT_SIM_HELPER");
  return env != nullptr && env[0] == '1';
}
bool ask(const vault::Challenge&) { return false; }
State poll() { return State::Idle; }
bool take(vault::Response&) { return false; }
void cancel() {}

bool sendCommand(MacCommand) { return false; }
bool takeMacState(MacState&) { return false; }

// CROSSPOINT_SIM_STATUS=1 hands pages 2 and 3 a plausible pair of boards, so
// they can be rendered in a simulator with no Mac to describe.
bool takeStatus() {
  static bool given = false;
  const char* env = std::getenv("CROSSPOINT_SIM_STATUS");
  if (given || env == nullptr || env[0] != '1') return false;
  given = true;
  struct Seed {
    StatusBoardId board;
    StatusCode code;
    const char* title;
    const char* detail;
  };
  static constexpr Seed kSeeds[] = {
      {StatusBoardId::Claude, StatusCode::AwaitingInput, "Add the stocks sleep screen", "xteink-x4-pro-2"},
      {StatusBoardId::Claude, StatusCode::InProcess, "Refactor the photo importer", "immich-tools"},
      {StatusBoardId::Claude, StatusCode::Completed, "Write the README", "dotfiles"},
      {StatusBoardId::Claude, StatusCode::Failed, "Migrate the database", "home-server"},
      {StatusBoardId::Services, StatusCode::Running, "Ambient tasks", "launchd"},
      {StatusBoardId::Services, StatusCode::Running, "Immich", "http"},
      {StatusBoardId::Services, StatusCode::Stopped, "Voice typing", "process"},
      {StatusBoardId::Services, StatusCode::Failed, "Wake TV", "exit 1"},
      {StatusBoardId::Services, StatusCode::Running, "Remote unlock", "this helper"},
  };
  for (const StatusBoardId board : {StatusBoardId::Claude, StatusBoardId::Services}) {
    uint8_t count = 0;
    for (const Seed& seed : kSeeds) count = static_cast<uint8_t>(count + (seed.board == board ? 1 : 0));
    uint8_t index = 0;
    for (const Seed& seed : kSeeds) {
      if (seed.board != board) continue;
      StatusRow row;
      row.status = seed.code;
      std::snprintf(row.title, sizeof(row.title), "%s", seed.title);
      std::snprintf(row.detail, sizeof(row.detail), "%s", seed.detail);
      uint8_t frame[kStatusFrameMax];
      const size_t len = encodeStatusRow(board, count, index++, row, frame, sizeof(frame));
      statusShown.feed(frame, len);
    }
  }
  return true;
}

const StatusAssembler& statusBoards() { return statusShown; }

// CROSSPOINT_SIM_NOWPLAYING="Title|Artist" hands the panel one song, so the
// now-playing row can be rendered and photographed in a simulator that has no
// radio to hear one. Given once, like a real track change.
bool takeNowPlaying(NowPlaying& out) {
  static bool given = false;
  if (given) return false;
  const char* env = std::getenv("CROSSPOINT_SIM_NOWPLAYING");
  if (env == nullptr || env[0] == '\0') return false;
  given = true;
  const char* bar = std::strchr(env, '|');
  const size_t titleLen = bar != nullptr ? static_cast<size_t>(bar - env) : std::strlen(env);
  out = NowPlaying{};
  out.state = NowPlayingState::Playing;
  std::snprintf(out.title, sizeof(out.title), "%.*s", static_cast<int>(titleLen), env);
  if (bar != nullptr) std::snprintf(out.artist, sizeof(out.artist), "%s", bar + 1);
  return true;
}

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
