#pragma once

// The remote's model: the seek profiles and the volume arithmetic.
//
// Freestanding C++17 -- no NimBLE, no renderer, no Activity -- so
// host-tests/remote builds it with a bare compiler. The radio lives in
// RemoteHid and the screen in RemoteScreens; this is the part with the
// judgement in it.
//
// ---------------------------------------------------------------------------
// Two decisions worth stating, both forced by what HID actually is.
//
// 1. SEEK IS A KEYSTROKE, NOT A MEDIA KEY. There is no HID usage meaning "skip
//    ten seconds". The transport has Fast Forward and Rewind, and on every
//    host that implements them they are scrub-while-held, not a jump. The ten
//    and five second jumps people mean belong to the PLAYER -- they are
//    YouTube's L and left-arrow, IINA's arrows -- so the seek buttons type the
//    player's own shortcut and a profile says which player. A remote that
//    promised "+10s" and sent a scrub would be lying on the button face.
//
// 2. THE VOLUME SLIDER IS RELATIVE, AND SAYS SO. HID sends volume UP and DOWN
//    steps; it cannot set a level and cannot read one back. macOS moves in
//    sixteenths, so the slider has seventeen positions and dragging from a to
//    b sends |b-a| steps -- exact, as long as the volume is only changed from
//    here. Change it on the Mac and this drifts, which is why dragging to
//    either end sends a full sixteen steps: that is the one move that ends in
//    a known state whatever the Mac was doing.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace remote {

// macOS volume is sixteen steps, so the slider is too: one notch here is one
// press of the volume key there, and the two cannot disagree about how far a
// drag went.
inline constexpr int kVolumeSteps = 16;

// How long fast-forward/rewind is held in the profile that uses them. Long
// enough that a host implementing scrub actually moves, short enough that a
// mis-tap is not a minute lost.
inline constexpr uint32_t kScrubHoldMs = 700;

// A keyboard chord, mirrored from RemoteHid::Chord so the core need not
// include the radio. Modifier bits: 1 LCtrl, 2 LShift, 4 LAlt, 8 LGui(Cmd).
struct KeyChord {
  uint8_t modifiers = 0;
  uint8_t key = 0;  // HID keyboard usage id; 0 means "use the media key instead"
};

// Which player the seek buttons are typing at.
enum class Profile : uint8_t {
  Browser,   // YouTube and friends: L forward, left-arrow back
  Player,    // IINA, QuickTime, VLC: the arrow keys
  MediaKey,  // no shortcut: hold the transport's own scrub keys
  Count,
};

// The name on the settings row.
const char* profileName(Profile profile);

// What the forward and back buttons send under this profile. A chord whose
// `key` is 0 means the caller should hold the media scrub key instead.
KeyChord forwardChord(Profile profile);
KeyChord backChord(Profile profile);

// The number beside the seek mark, or nullptr when this profile cannot keep
// one -- the host decides how far a scrub goes, so the button is the mark
// alone rather than a number it would be inventing.
const char* forwardSeconds(Profile profile);
const char* backSeconds(Profile profile);

// Clamps a slider position to 0..kVolumeSteps.
int clampVolume(int position);

// How many volume-up (positive) or volume-down (negative) presses move the
// host from `from` to `to`. Dragging to an end returns a full sixteen steps in
// that direction regardless of where it thought it was -- that is the move
// that resyncs a slider the Mac has drifted away from.
int volumeStepsBetween(int from, int to);

}  // namespace remote
