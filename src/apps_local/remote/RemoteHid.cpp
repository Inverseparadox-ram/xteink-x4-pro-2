#include "RemoteHid.h"

#include <Logging.h>

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
    case Key::Play:
      return 0x00B0;
    case Key::Pause:
      return 0x00B1;
    case Key::Stop:
      return 0x00B7;
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

// Press then release. A host that sees a press and never a release treats the
// key as stuck, which on a volume key means the volume keeps moving.
bool tapConsumer(const uint16_t usage) {
  if (usage == 0) return false;
  const uint8_t press[2] = {static_cast<uint8_t>(usage & 0xFF), static_cast<uint8_t>(usage >> 8)};
  const uint8_t release[2] = {0, 0};
  if (!notify(consumerIn, press, sizeof(press))) return false;
  delay(12);
  notify(consumerIn, release, sizeof(release));
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

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->getHidService()->getUUID());
  advertising->enableScanResponse(true);
  server->start();
  advertising->start();
  LOG_INF(kTag, "advertising as '%s'", kName);
}

void end() {
  if (!started) return;
  started = false;
  hostConnected = false;
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
  uint8_t press[8] = {chord.modifiers, 0, chord.key, 0, 0, 0, 0, 0};
  const uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (!notify(keyboardIn, press, sizeof(press))) return false;
  delay(12);
  notify(keyboardIn, release, sizeof(release));
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
void end() {}
Link link() { return Link::Advertising; }
bool ready() { return false; }
bool send(Key) { return false; }
bool sendChord(const Chord&) { return false; }
bool hold(Key, uint32_t) { return false; }
void setBattery(uint8_t) {}
void forgetPairings() {}

#endif

}  // namespace remote
