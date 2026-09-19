#include "RemoteCore.h"

#include <cstdio>

namespace remote {
namespace {

// HID keyboard usage ids (HUT 1.12, section 10).
constexpr uint8_t kKeyL = 0x0F;
constexpr uint8_t kKeyJ = 0x0D;
constexpr uint8_t kKeyRight = 0x4F;
constexpr uint8_t kKeyLeft = 0x50;

}  // namespace

const char* profileName(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      return "YOUTUBE / BROWSER";
    case Profile::Player:
      return "IINA / QUICKTIME / VLC";
    case Profile::MediaKey:
      return "ANY PLAYER (SCRUB)";
    case Profile::Count:
      break;
  }
  return "UNKNOWN";
}

const char* profileNote(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      // The one profile whose numbers are exact, because YouTube defines them:
      // L is ten seconds on, the left arrow is five seconds back.
      return "Types L and left-arrow: 10s and 5s in YouTube.";
    case Profile::Player:
      return "Types the arrow keys. The player sets the jump.";
    case Profile::MediaKey:
      return "Holds scrub. Works anywhere; the host sets how far.";
    case Profile::Count:
      break;
  }
  return "";
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

const char* forwardLabel(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      return "+10s";
    case Profile::Player:
      // The arrow keys jump whatever the player is set to, so the button does
      // not claim a number it cannot keep.
      return "FWD";
    case Profile::MediaKey:
      return "SCRUB >>";
    case Profile::Count:
      break;
  }
  return "FWD";
}

const char* backLabel(const Profile profile) {
  switch (profile) {
    case Profile::Browser:
      return "-5s";
    case Profile::Player:
      return "BACK";
    case Profile::MediaKey:
      return "<< SCRUB";
    case Profile::Count:
      break;
  }
  return "BACK";
}

int clampVolume(const int position) {
  if (position < 0) return 0;
  if (position > kVolumeSteps) return kVolumeSteps;
  return position;
}

int volumeStepsBetween(const int from, const int to) {
  const int a = clampVolume(from);
  const int b = clampVolume(to);
  // An end stop resyncs. The remote's count is a guess the moment anyone
  // touches the Mac's own volume, and sending a full sixteen at an end is the
  // one move that lands on a level both sides agree about.
  if (b == 0) return -kVolumeSteps;
  if (b == kVolumeSteps) return kVolumeSteps;
  return b - a;
}

const char* formatVolume(const int position, char* out, const size_t size) {
  // "VOLUME 11 / 16" -- the remote's own count, named as such. It is not the
  // Mac's volume and the caption must not read as though it were.
  std::snprintf(out, size, "VOLUME %d / %d", clampVolume(position), kVolumeSteps);
  return out;
}

}  // namespace remote
