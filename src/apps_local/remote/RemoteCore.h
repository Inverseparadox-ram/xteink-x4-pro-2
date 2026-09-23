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

#include <cstddef>
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

// --- Now playing ---------------------------------------------------------
//
// HID is one-way, so the remote could never know the track -- until the unlock
// helper put an agent on the Mac with a GATT channel back. The helper watches
// the notifications Music and Spotify broadcast and writes one of these frames
// when the answer changes:
//
//   [0] version   kNowPlayingVersion
//   [1] state     NowPlayingState
//   [2] n         title length, at most kNowPlayingFieldMax
//   [3..3+n)      title, UTF-8
//   [3+n] m       artist length, at most kNowPlayingFieldMax
//   [..+m)        artist, UTF-8
//
// Display only, and NOT a security decision, so it carries no MAC -- the worst
// a forged frame can do is print a wrong song. The characteristic requires an
// encrypted link, which in practice means the bonded Mac and nobody else.

inline constexpr uint8_t kNowPlayingVersion = 1;
inline constexpr size_t kNowPlayingFieldMax = 64;
inline constexpr size_t kNowPlayingFrameMax = 3 + kNowPlayingFieldMax + 1 + kNowPlayingFieldMax;

enum class NowPlayingState : uint8_t {
  Nothing = 0,  // nothing playing, or the helper has not heard from a player
  Playing = 1,
  Paused = 2,
};

struct NowPlaying {
  NowPlayingState state = NowPlayingState::Nothing;
  char title[kNowPlayingFieldMax + 1] = {};
  char artist[kNowPlayingFieldMax + 1] = {};

  // Worth drawing: something is playing or paused AND there is a title. A
  // player that reports Playing with no name has nothing to say.
  bool present() const { return state != NowPlayingState::Nothing && title[0] != '\0'; }
};

// False on anything that is not exactly one well-formed frame, and `out` is
// then left untouched. A frame the reader cannot parse must not blank the line
// that was on screen, and it must not half-overwrite it either.
bool decodeNowPlaying(const uint8_t* data, size_t len, NowPlaying& out);

// The Mac's half, kept here so the tests pin the exact bytes the Swift helper
// has to produce. Fields longer than kNowPlayingFieldMax are cut at a character
// boundary, never through the middle of one.
size_t encodeNowPlaying(const NowPlaying& in, uint8_t* out, size_t size);

// How many of the first `len` bytes of `text` end on a complete UTF-8
// character. A title cut at a byte limit can split a multi-byte character, and
// half a character is a glyph the font draws as garbage.
size_t utf8CompleteLength(const char* text, size_t len);

bool sameNowPlaying(const NowPlaying& a, const NowPlaying& b);

}  // namespace remote
