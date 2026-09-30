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
// 3. THE THIRD ROW IS THREE DIFFERENT KINDS OF BUTTON. F8 is a plain key held
//    down across two taps, which HID does natively. The microphone and the
//    padlock need something HID cannot do at all -- an answer from the Mac --
//    so they ride the unlock helper's GATT service. See RemoteVault.h.
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

// --- The hold button ------------------------------------------------------
//
// One tap presses F8 and leaves it down; the next lets it go. That is what a
// push-to-talk or dictation app bound to a held key wants, without anyone
// having to keep a thumb on the panel. HID keyboard usage 0x41.
inline constexpr uint8_t kHoldKeyUsage = 0x41;

// --- The microphone button -----------------------------------------------
//
// Reader -> Mac, over the helper's COMMAND characteristic: [version, command].
// Mac -> reader, over MACSTATE: [version, flags]. There is no HID usage for a
// microphone and no keyboard shortcut that silences every one, so only the
// helper can do this.
//
// NOT authenticated beyond the Bluetooth bond, deliberately: it has to work
// without the Mac verifying anything, and the bond already means the only
// central listening is the Mac that paired. The worst a hijacked command can
// do is change whether the Mac's microphones are muted.

inline constexpr uint8_t kMacLinkVersion = 1;

enum class MacCommand : uint8_t {
  MuteMicrophones = 0x01,
  UnmuteMicrophones = 0x02,
};

inline constexpr size_t kCommandLen = 2;
void encodeCommand(MacCommand command, uint8_t out[kCommandLen]);
bool decodeCommand(const uint8_t* data, size_t len, MacCommand& out);

// What the Mac reports about itself. One bit for now, a byte so it can grow
// without a new characteristic.
struct MacState {
  bool known = false;             // nothing heard yet: the button claims nothing
  bool microphonesMuted = false;  // every input device the Mac has is muted
};

inline constexpr size_t kMacStateLen = 2;
inline constexpr uint8_t kMacStateMicMuted = 0x01;
void encodeMacState(const MacState& state, uint8_t out[kMacStateLen]);
bool decodeMacState(const uint8_t* data, size_t len, MacState& out);

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

// --- Status boards --------------------------------------------------------
//
// Pages 2 and 3 of the remote: what the Claude Code sessions on the Mac are
// doing, and whether the Mac's own background services are up. The helper
// sends a board as ROWS, one GATT write each, so no single write is large and
// a board can grow without a bigger characteristic:
//
//   [0] version  kStatusVersion
//   [1] board    StatusBoardId
//   [2] count    rows in this board, at most kStatusRowsMax (0 = empty)
//   [3] index    this row, 0..count-1 (0 in an empty board)
//   [4] status   StatusCode
//   [5] n        title length, at most kStatusTitleMax
//   [6..6+n)     title, UTF-8
//   [6+n] m      detail length, at most kStatusDetailMax
//   [..+m)       detail, UTF-8
//
// A board is shown only once its LAST row has arrived, so the page never draws
// half of one board over half of another. Display only, like now playing: the
// characteristic needs an encrypted link, and a forged row prints a wrong word.

inline constexpr uint8_t kStatusVersion = 1;
inline constexpr size_t kStatusRowsMax = 10;
inline constexpr size_t kStatusTitleMax = 48;
inline constexpr size_t kStatusDetailMax = 32;
inline constexpr size_t kStatusFrameMax = 6 + kStatusTitleMax + 1 + kStatusDetailMax;

enum class StatusBoardId : uint8_t { Claude = 1, Services = 2 };

enum class StatusCode : uint8_t {
  Unknown = 0,
  InProcess = 1,      // Claude is working on it
  AwaitingInput = 2,  // Claude asked something: a permission, or it is idle on a prompt
  Completed = 3,      // the turn finished
  Failed = 4,         // the session died mid-turn, or the service exited with an error
  Running = 5,
  Stopped = 6,
};

// The word the page prints: "in process", "awaiting input", "completed"...
const char* statusWord(StatusCode code);

// Whether the row wants a person: the page draws these in reverse.
bool statusNeedsAttention(StatusCode code);

struct StatusRow {
  StatusCode status = StatusCode::Unknown;
  char title[kStatusTitleMax + 1] = {};
  char detail[kStatusDetailMax + 1] = {};
};

struct StatusBoard {
  bool known = false;  // a whole board has arrived since the helper connected
  uint8_t count = 0;
  StatusRow rows[kStatusRowsMax];
};

size_t encodeStatusRow(StatusBoardId board, uint8_t count, uint8_t index, const StatusRow& row, uint8_t* out,
                       size_t size);

// Collects rows into boards. feed() returns true when a board just became
// complete; take it with board(). A row out of order, from a different count,
// or malformed abandons the board being collected rather than guessing.
class StatusAssembler {
 public:
  bool feed(const uint8_t* data, size_t len);
  const StatusBoard& board(StatusBoardId id) const;
  void forget();  // the helper went away: nothing is known any more

 private:
  StatusBoard shown_[2];
  StatusBoard pending_[2];
  uint8_t expected_[2] = {0, 0};  // the next index each pending board wants
};

}  // namespace remote
