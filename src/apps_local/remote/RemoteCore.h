#pragma once

// The remote's model: the seek profiles and the three Mac shortcuts.
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
// 2. VOLUME IS TWO BUTTONS, NOT A SLIDER. HID sends volume UP and DOWN steps;
//    it cannot set a level and cannot read one back. A slider therefore drew a
//    position the remote had guessed, and the guess was wrong the moment
//    anyone touched the volume on the Mac. Two buttons claim nothing: each tap
//    is one step, which is exactly what the wire carries.
//
// 3. THE SHORTCUT BUTTONS TYPE WHAT A MAC ALREADY UNDERSTANDS. There is no HID
//    usage for Siri and none for launching an application, so both are
//    keyboard shortcuts a stock Mac already has, sent as chords. Nothing has
//    to be bound first. The unlock button beside them is the exception that
//    needed a protocol rather than a chord -- see RemoteVault.h.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace remote {

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

// --- The shortcut buttons -------------------------------------------------
//
// Each is a chord the Mac already knows, or is told once.

// How long Command-Space is held for Siri. macOS reads a HELD Command-Space as
// Siri and a TAPPED one as Spotlight -- the same distinction the Mac's own
// keyboard makes -- so this is the only thing separating the two buttons.
// A second is what "Hold Command Space" means in System Settings; 1200ms
// leaves margin for the BLE round trip at either end.
inline constexpr uint32_t kSiriHoldMs = 1200;

// Command-Space. Held it is Siri, tapped it is Spotlight.
KeyChord commandSpace();

// What the Claude button types into Spotlight after opening it. Spotlight is
// the one route to an application that needs nothing set up on the Mac first.
inline constexpr const char* kClaudeQuery = "claude";

}  // namespace remote
