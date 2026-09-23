#include "RemoteCore.h"

namespace remote {
namespace {

// HID keyboard usage ids (HUT 1.12, section 10).
constexpr uint8_t kKeyL = 0x0F;
constexpr uint8_t kKeyJ = 0x0D;
constexpr uint8_t kKeyRight = 0x4F;
constexpr uint8_t kKeyLeft = 0x50;
constexpr uint8_t kKeySpace = 0x2C;

// Modifier bits, mirrored from RemoteHid::Chord.
constexpr uint8_t kModCtrl = 1;
constexpr uint8_t kModAlt = 4;
constexpr uint8_t kModCmd = 8;

}  // namespace

const char* profileName(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      return "YOUTUBE";
    case Profile::Player:
      return "PLAYER KEYS";
    case Profile::MediaKey:
      return "SCRUB";
    case Profile::Count:
      break;
  }
  return "UNKNOWN";
}

KeyChord forwardChord(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      return KeyChord{0, kKeyL};
    case Profile::Player:
      return KeyChord{0, kKeyRight};
    case Profile::MediaKey:
    case Profile::Count:
      break;
  }
  return KeyChord{};  // no chord: the caller holds the media scrub key
}

KeyChord backChord(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      // The left arrow, not J. J is ten seconds back and the ask was five, and
      // five back with ten forward is the asymmetry people actually use: you
      // overshoot forward and nudge back.
      return KeyChord{0, kKeyLeft};
    case Profile::Player:
      return KeyChord{0, kKeyLeft};
    case Profile::MediaKey:
    case Profile::Count:
      break;
  }
  return KeyChord{};
}

const char* forwardSeconds(const Profile profile) {
  // Only YouTube defines the numbers, so only YouTube prints them. Under the
  // others the mark stands alone: "FWD" was a word doing an arrow's job, and
  // an arrow was already there.
  return profile == Profile::Browser ? "10" : nullptr;
}

const char* backSeconds(const Profile profile) { return profile == Profile::Browser ? "5" : nullptr; }

KeyChord commandSpace() { return KeyChord{kModCmd, kKeySpace}; }

}  // namespace remote
