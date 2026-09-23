#include "RemoteHid.h"

#include <Logging.h>

#include <cstdlib>

#include "RemoteLink.h"

#if defined(CROSSPLAY_BLE_HID)
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#endif

namespace remote {
namespace {

constexpr const char* kTag = "REMOTE";
constexpr const char* kName = "CrossPlay Remote";

// Two reports, because the job needs two kinds of key.
//
// The CONSUMER report carries the media keys -- play, next, volume -- which is
// what every host understands without being told which player is running.
//
// The KEYBOARD report exists for seek. There is no HID usage meaning "forward
// ten seconds": fast-forward and rewind are scrub-while-held, and the ten and
// five second jumps people actually want are defined by the PLAYER (YouTube's
// L and left-arrow, IINA's arrows, and so on). So the seek buttons type the
// player's own shortcut, and the profile says which.
constexpr uint8_t kReportConsumer = 1;
constexpr uint8_t kReportKeyboard = 2;

#if defined(CROSSPLAY_BLE_HID)

// A 16-bit usage array for the consumer control, and a standard 8-byte
// keyboard. The consumer report is an array rather than a button bitmap
// because a bitmap fixes the set of controls in the descriptor, and this app's
// set is decided by the screens.
const uint8_t kReportMap[] = {
    // --- Consumer control -------------------------------------------------
    0x05, 0x0C,             // Usage Page (Consumer)
    0x09, 0x01,             // Usage (Consumer Control)
    0xA1, 0x01,             // Collection (Application)
    0x85, kReportConsumer,  //   Report ID
    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x03,       //   Logical Maximum (0x3FF)
    0x19, 0x00,             //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,       //   Usage Maximum (0x3FF)
    0x75, 0x10,             //   Report Size (16)
    0x95, 0x01,             //   Report Count (1)
    0x81, 0x00,             //   Input (Data, Array, Absolute)
    0xC0,                   // End Collection

    // --- Keyboard ---------------------------------------------------------
    0x05, 0x01,              // Usage Page (Generic Desktop)
    0x09, 0x06,              // Usage (Keyboard)
    0xA1, 0x01,              // Collection (Application)
    0x85, kReportKeyboard,   //   Report ID
    0x05, 0x07,              //   Usage Page (Keyboard)
    0x19, 0xE0,              //   Usage Minimum (Left Control)
    0x29, 0xE7,              //   Usage Maximum (Right GUI)
    0x15, 0x00, 0x25, 0x01,  //   Logical 0..1
    0x75, 0x01, 0x95, 0x08,  //   8 bits
    0x81, 0x02,              //   Input (Data, Variable, Absolute) - modifiers
    0x95, 0x01, 0x75, 0x08,  //   1 byte
    0x81, 0x03,              //   Input (Constant) - reserved
    0x95, 0x06, 0x75, 0x08,  //   6 bytes
    0x15, 0x00, 0x25, 0x65,  //   Logical 0..101
    0x05, 0x07,              //   Usage Page (Keyboard)
    0x19, 0x00, 0x29, 0x65,  //   Usage Minimum/Maximum
    0x81, 0x00,              //   Input (Data, Array) - keys
    0xC0                     // End Collection
};

// HID consumer usage ids (HUT 1.12, section 15). macOS honours all of these;
// the toggle at 0xCD is the one it honours most consistently, which is why the
// screens lead with it.
uint16_t usageFor(const Key key) {
  switch (key) {
    case Key::PlayPause:
      return 0x00CD;
    case Key::Next:
      return 0x00B5;
    case Key::Previous:
      return 0x00B6;
    case Key::VolumeUp:
      return 0x00E9;
    case Key::VolumeDown:
      return 0x00EA;
    case Key::Mute:
      return 0x00E2;
    case Key::FastForward:
      return 0x00B3;
    case Key::Rewind:
      return 0x00B4;
  }
  return 0;
}

NimBLEHIDDevice* hid = nullptr;
NimBLECharacteristic* consumerIn = nullptr;
NimBLECharacteristic* keyboardIn = nullptr;
NimBLEServer* server = nullptr;
bool started = false;
volatile bool hostConnected = false;

// Advertising has to be restarted by hand after a disconnect or the remote is
// invisible until the app is reopened -- which reads as "it broke".
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    hostConnected = true;
    LOG_INF(kTag, "host connected");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    hostConnected = false;
    LOG_INF(kTag, "host disconnected (%d); advertising again", reason);
    NimBLEDevice::startAdvertising();
  }
};

ServerCallbacks serverCallbacks;

bool notify(NimBLECharacteristic* characteristic, const uint8_t* data, const size_t len) {
  if (characteristic == nullptr || !hostConnected) return false;
  characteristic->setValue(data, len);
  characteristic->notify();
  return true;
}

// How long a key is held before the release report goes out.
//
// THIS NUMBER IS THE MUTE BUG. It was 12ms, which is shorter than the BLE
// connection interval macOS negotiates (typically 15-30ms), and a notify() only
// leaves the device when its interval comes round. Two notifies inside one
// interval means the second setValue() overwrites the first before either is
// transmitted, so the host sees the release and never the press.
//
// That is why the symptom picked on Mute specifically. A drag of the volume
// slider fired sixteen reports, so enough of them survived for the volume to
// visibly move; play/pause was retried by anyone who thought they had missed.
// Mute is one tap with one report, and a lost report is a button that does
// nothing.
//
// 45ms clears the widest interval macOS uses with room to spare, and is still
// shorter than a human notices between tapping a button and hearing the
// result.
constexpr uint32_t kKeyHoldMs = 45;

// Press then release. A host that sees a press and never a release treats the
// key as stuck, which on a volume key means the volume keeps moving.
bool tapConsumer(const uint16_t usage) {
  if (usage == 0) return false;
  const uint8_t press[2] = {static_cast<uint8_t>(usage & 0xFF), static_cast<uint8_t>(usage >> 8)};
  const uint8_t release[2] = {0, 0};
  if (!notify(consumerIn, press, sizeof(press))) return false;
  delay(kKeyHoldMs);
  notify(consumerIn, release, sizeof(release));
  // The gap matters as much as the hold: back-to-back taps race the same way a
  // press and its own release did.
  delay(kKeyHoldMs);
  return true;
}

// US-layout HID keyboard usage for the characters Spotlight needs. Letters and
// digits only -- the one string this app types is an application name.
uint8_t keyboardUsageFor(const char c) {
  if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(0x04 + (c - 'a'));
  if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(0x04 + (c - 'A'));
  if (c >= '1' && c <= '9') return static_cast<uint8_t>(0x1E + (c - '1'));
  if (c == '0') return 0x27;
  if (c == ' ') return 0x2C;
  return 0;
}

// US-layout HID keyboard usage plus the shift flag, for every printable ASCII
// character. A password is not an application name: it has symbols in it, and
// keyboardUsageFor() above deliberately knows only letters and digits.
struct KeyStroke {
  uint8_t usage;
  bool shift;
};

KeyStroke strokeFor(const char c) {
  if (c >= 'a' && c <= 'z') return {static_cast<uint8_t>(0x04 + (c - 'a')), false};
  if (c >= 'A' && c <= 'Z') return {static_cast<uint8_t>(0x04 + (c - 'A')), true};
  if (c >= '1' && c <= '9') return {static_cast<uint8_t>(0x1E + (c - '1')), false};
  if (c == '0') return {0x27, false};
  // The shifted number row, in the order the keys sit in.
  static const char* kShiftedDigits = ")!@#$%^&*(";
  for (int i = 0; i < 10; ++i) {
    if (c == kShiftedDigits[i]) return {static_cast<uint8_t>(i == 0 ? 0x27 : 0x1E + (i - 1)), true};
  }
  switch (c) {
    case ' ':
      return {0x2C, false};
    case '-':
      return {0x2D, false};
    case '_':
      return {0x2D, true};
    case '=':
      return {0x2E, false};
    case '+':
      return {0x2E, true};
    case '[':
      return {0x2F, false};
    case '{':
      return {0x2F, true};
    case ']':
      return {0x30, false};
    case '}':
      return {0x30, true};
    case '\\':
      return {0x31, false};
    case '|':
      return {0x31, true};
    case ';':
      return {0x33, false};
    case ':':
      return {0x33, true};
    case '\'':
      return {0x34, false};
    case '"':
      return {0x34, true};
    case '`':
      return {0x35, false};
    case '~':
      return {0x35, true};
    case ',':
      return {0x36, false};
    case '<':
      return {0x36, true};
    case '.':
      return {0x37, false};
    case '>':
      return {0x37, true};
    case '/':
      return {0x38, false};
    case '?':
      return {0x38, true};
    default:
      return {0, false};
  }
}

bool tapKeyboard(const uint8_t modifiers, const uint8_t key) {
  uint8_t press[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
  const uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (!notify(keyboardIn, press, sizeof(press))) return false;
  delay(kKeyHoldMs);
  notify(keyboardIn, release, sizeof(release));
  delay(kKeyHoldMs);
  return true;
}

#endif  // CROSSPLAY_BLE_HID

}  // namespace

const char* deviceName() { return kName; }

#if defined(CROSSPLAY_BLE_HID)

void begin() {
  if (started) return;
  started = true;

  NimBLEDevice::init(kName);
  // Bonding on, MITM off, secure connections on: macOS pairs a HID peripheral
  // without a passkey prompt this way, and the bond is what lets it reconnect
  // by itself afterwards. Requiring MITM would demand a passkey UI this device
  // has no reason to grow.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);

  hid = new NimBLEHIDDevice(server);
  hid->setManufacturer("CrossPlay");
  // Vendor 0x05AC is Apple. Claiming it makes macOS treat the device as a
  // media keyboard it already understands rather than an unknown peripheral,
  // which is the difference between the media keys working and being ignored.
  hid->setPnp(0x02, 0x05AC, 0x820A, 0x0100);
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));

  consumerIn = hid->getInputReport(kReportConsumer);
  keyboardIn = hid->getInputReport(kReportKeyboard);

  // The unlock button's own service, BEFORE the server starts: NimBLE
  // registers services at start, and one added afterwards is a service the
  // host never discovers.
  helper::begin();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->getHidService()->getUUID());
  advertising->enableScanResponse(true);
  server->start();
  advertising->start();

  // AFTER start(), which fills the scan response from its own stored data when
  // it has not been set yet -- doing this first would have it overwritten.
  // The Mac's helper scans for this UUID when it cannot find the reader among
  // the peripherals macOS has already connected.
  NimBLEAdvertisementData scanResponse;
  scanResponse.setName(kName);
  scanResponse.addServiceUUID(NimBLEUUID(helper::serviceUuid()));
  advertising->setScanResponseData(scanResponse);
  LOG_INF(kTag, "advertising as '%s'", kName);
}

void end() {
  if (!started) return;
  started = false;
  hostConnected = false;
  // Before deinit, which frees the service this points at.
  helper::end();
  NimBLEDevice::deinit(true);
  hid = nullptr;
  consumerIn = nullptr;
  keyboardIn = nullptr;
  server = nullptr;
  LOG_INF(kTag, "radio down");
}

Link link() {
  if (!started) return Link::Off;
  return hostConnected ? Link::Connected : Link::Advertising;
}

bool ready() { return started && hostConnected; }

bool send(const Key key) { return tapConsumer(usageFor(key)); }

bool sendChord(const Chord& chord) {
  if (chord.key == 0) return false;
  return tapKeyboard(chord.modifiers, chord.key);
}

bool holdChord(const Chord& chord, const uint32_t ms) {
  if (chord.key == 0) return false;
  uint8_t press[8] = {chord.modifiers, 0, chord.key, 0, 0, 0, 0, 0};
  const uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (!notify(keyboardIn, press, sizeof(press))) return false;
  delay(ms);
  notify(keyboardIn, release, sizeof(release));
  delay(kKeyHoldMs);
  return true;
}

bool typeText(const char* text) {
  if (text == nullptr) return false;
  for (const char* c = text; *c != '\0'; ++c) {
    const uint8_t usage = keyboardUsageFor(*c);
    // A character with no usage is skipped rather than aborting: the caller is
    // typing an application name, and a name that loses a stray character
    // still lands on the right Spotlight hit.
    if (usage == 0) continue;
    if (!tapKeyboard(0, usage)) return false;
  }
  return true;
}

bool sendReturn() { return tapKeyboard(0, 0x28); }

bool typeSecret(const char* text) {
  if (text == nullptr) return false;
  for (const char* c = text; *c != '\0'; ++c) {
    const KeyStroke stroke = strokeFor(*c);
    // A character with no key is an ABORT, not a skip: typeText() may lose a
    // stray character out of an application name and still land on the right
    // hit, but a password with a character missing is a failed attempt the Mac
    // counts against the account.
    if (stroke.usage == 0) {
      LOG_ERR(kTag, "unlock: no US-layout key for a character; typing nothing");
      return false;
    }
    if (!tapKeyboard(stroke.shift ? 0x02 : 0x00, stroke.usage)) return false;
  }
  return true;
}

bool wakeHost() {
  // Left shift held down and let go, with no key on the report. Nothing is
  // typed, and a display that was asleep is awake by the time the password
  // starts.
  const uint8_t press[8] = {0x02, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (!notify(keyboardIn, press, sizeof(press))) return false;
  delay(kKeyHoldMs);
  notify(keyboardIn, release, sizeof(release));
  delay(kKeyHoldMs);
  return true;
}

bool hold(const Key key, const uint32_t ms) {
  const uint16_t usage = usageFor(key);
  if (usage == 0) return false;
  const uint8_t press[2] = {static_cast<uint8_t>(usage & 0xFF), static_cast<uint8_t>(usage >> 8)};
  const uint8_t release[2] = {0, 0};
  if (!notify(consumerIn, press, sizeof(press))) return false;
  delay(ms);
  notify(consumerIn, release, sizeof(release));
  delay(kKeyHoldMs);
  return true;
}

void setBattery(const uint8_t percent) {
  if (hid != nullptr) hid->setBatteryLevel(percent > 100 ? 100 : percent, true);
}

void forgetPairings() {
  NimBLEDevice::deleteAllBonds();
  LOG_INF(kTag, "bonds cleared");
}

#else  // simulator: no radio, so the screens can still be driven and rendered.

void begin() { LOG_INF(kTag, "sim: no radio; pretending to advertise"); }
void end() { helper::end(); }
// CROSSPOINT_SIM_REMOTE_CONNECTED=1 draws the connected panel, which is the only
// one the now-playing row appears on.
Link link() {
  const char* env = std::getenv("CROSSPOINT_SIM_REMOTE_CONNECTED");
  return env != nullptr && env[0] == '1' ? Link::Connected : Link::Advertising;
}
bool ready() { return false; }
bool send(Key) { return false; }
bool sendChord(const Chord&) { return false; }
bool holdChord(const Chord&, uint32_t) { return false; }
bool typeText(const char*) { return false; }
bool sendReturn() { return false; }
bool typeSecret(const char*) { return false; }
bool wakeHost() { return false; }
bool hold(Key, uint32_t) { return false; }
void setBattery(uint8_t) {}
void forgetPairings() {}

#endif

}  // namespace remote
