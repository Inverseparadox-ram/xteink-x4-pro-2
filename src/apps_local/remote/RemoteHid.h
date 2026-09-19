#pragma once

// The radio half of the media remote: a BLE HID device the Mac pairs with.
//
// ---------------------------------------------------------------------------
// Why HID, and what that buys and costs.
//
// The X4 Pro cannot play audio. Its ESP32-S3 has no Bluetooth Classic, so no
// A2DP -- Espressif ships esp_a2dp_api.h for the original ESP32 and for no
// other chip -- and the board declares no audio output at all
// (FREEINK_CAP_AUDIO is Murphy and M5; this device has not even a buzzer).
//
// So the device does not carry the music. It carries the CONTROLS. Paired as a
// BLE keyboard, it sends the same key presses a media keyboard sends, and
// whatever the Mac is already playing -- to a Bluetooth speaker, to AirPods,
// to anything -- obeys them.
//
// What that costs is metadata. HID is one-way: reports go out, nothing comes
// back. The remote cannot know the track, the volume, or even whether music is
// playing. Every screen in this app is written around that fact rather than
// pretending otherwise. (iOS has AMS, a BLE service that would answer those
// questions. macOS does not expose it.)
//
// NimBLE, not Bluedroid: these prebuilt Arduino libs are
// CONFIG_BT_NIMBLE_ENABLED, which is why platformio.ini's base ignores the
// Arduino BLE library entirely.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace remote {

// What a control does, in the app's own vocabulary. The mapping to HID lives
// in the .cpp so the screens never name a usage code.
enum class Key : uint8_t {
  PlayPause,  // the toggle, and the one macOS honours most reliably
  Next,
  Previous,
  VolumeUp,
  VolumeDown,
  Mute,
  // Scrub. These are press-and-hold on every host that implements them at
  // all, which is why send() is the wrong call for them and hold() is right.
  FastForward,
  Rewind,
};

// Where the seek buttons send their keystrokes. macOS has no HID code meaning
// "skip ten seconds" -- that idea belongs to the player, not the transport --
// so the seek buttons send the keyboard shortcut the chosen player uses. See
// RemoteCore.h for the profiles.
struct Chord {
  uint8_t modifiers = 0;  // bitmask: 1 LCtrl, 2 LShift, 4 LAlt, 8 LGui
  uint8_t key = 0;        // HID keyboard usage id, 0 for "nothing to send"
};

// Connection state, as much of it as a peripheral can know.
enum class Link : uint8_t {
  Off,          // the radio has not been started
  Advertising,  // discoverable, nothing connected
  Connected,    // a host is connected and reports are going out
};

// Brings the radio up and starts advertising. Safe to call twice.
void begin();

// Takes the radio down. Called when the app closes, so BLE is not left on for
// a device whose whole point is lasting weeks on a charge.
void end();

Link link();

// True once a host has connected AND subscribed to the input reports. A report
// sent before that goes nowhere, which on screen is indistinguishable from a
// remote that does not work.
bool ready();

// The name the Mac shows in its Bluetooth list.
const char* deviceName();

// Sends a media key: the press and the release, because the host needs both.
// False when there is no subscribed host, which the screen reports rather than
// swallowing.
bool send(Key key);

// Sends a keyboard chord the same way. Used for seek and for the three
// shortcut buttons, none of which has a media key.
bool sendChord(const Chord& chord);

// Holds a chord down for `ms`. macOS distinguishes Siri from Spotlight by how
// long Command-Space is held, so the difference between the two is this call
// and sendChord().
bool holdChord(const Chord& chord, uint32_t ms);

// Types `text` as keystrokes, US layout, letters and digits only. Used to
// drive Spotlight, which is the one path to an application that needs no
// shortcut set up on the Mac first.
bool typeText(const char* text);

// Taps Return.
bool sendReturn();

// Holds a media key down for `ms` and then releases it. Fast-forward and
// rewind are press-and-hold scrub controls on the hosts that implement them at
// all; a tap of one is usually ignored.
bool hold(Key key, uint32_t ms);

// Tells the host what is left in the battery, which macOS shows beside the
// device. Cheap, and it is the one thing a remote CAN report back.
void setBattery(uint8_t percent);

// Forgets every bonded host, so the Mac can be paired again from scratch after
// a failed pairing -- the usual cure for a BLE device the host thinks it knows
// and no longer trusts.
void forgetPairings();

}  // namespace remote
