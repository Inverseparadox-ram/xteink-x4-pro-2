#pragma once

// The remote's screens. Freestanding builders in the fork's usual mould: a
// model in, a drawn frame out, so host-tests/ui can assert what they drew and
// what they made tappable.
//
// ---------------------------------------------------------------------------
// The layout, and the one thing it refuses to do.
//
// It never shows a state it cannot know. HID is one-way -- reports go out and
// nothing comes back -- so this screen has no idea whether music is playing,
// what the track is, or where the Mac's volume really sits. Every control is
// therefore a VERB, not a state: volume is two buttons that each send one
// step, rather than a slider drawing a position the remote had guessed at.
//
// The transport row is the exception that earns its size: PREV, PLAY/PAUSE and
// NEXT are what a hand reaches for without looking, so they get the biggest
// targets on the panel and the top of the body.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "../ui/ToyboxScreen.h"

namespace remoteui {

namespace fui = freeink::ui;

// Chess 1-4, link 200s, Hacker News 300s, Instapaper 320s, Notes 340s,
// Weather 360s. The remote takes the 380s.
enum : fui::ActionId {
  ActionPlayPause = 380,
  ActionNext = 381,
  ActionPrevious = 382,
  ActionSiri = 383,
  ActionClaude = 384,
  ActionUnlock = 385,
  ActionForward = 386,
  ActionBack = 387,
  ActionVolumeUp = 388,
  ActionMute = 389,
  ActionProfile = 390,
  ActionForget = 391,
  ActionForgetConfirm = 392,
  ActionForgetCancel = 393,
  ActionVolumeDown = 394,
  // The PIN pad. The digit rides on the action's value the way the comic
  // number's does, so ten keys cost one action and not ten.
  ActionPinDigit = 395,
  ActionPinBack = 396,
  ActionPinOk = 397,
  ActionPairDone = 398,
};

// Which of the three faces the unlock button is wearing. It is the LAST
// VERIFIED answer, never a local guess: a reader that assumed the Mac was
// still locked because it locked it would type the password into an open
// session the first time anyone touched the keyboard.
enum class UnlockFace : uint8_t {
  Ask,     // nothing verified -- a question, and a tap asks
  Unlock,  // the Mac said it is locked
  Lock,    // the Mac said it is awake
};

struct RemoteModel {
  // The link state is drawn as a mark. The only words are the ones no mark can
  // carry: where to look on the Mac when it cannot find the device, and why
  // the unlock button refused. Set it and the row becomes that sentence; leave
  // it empty and the row is the mark alone.
  const char* pairingHint = "";
  bool connected = false;

  // "10" and "5", or null under a profile that cannot promise seconds -- then
  // the seek buttons are their circular arrows and nothing else.
  const char* forwardSeconds = nullptr;
  const char* backSeconds = nullptr;

  // The one piece of state the remote is entitled to remember: that IT sent a
  // mute. The button's own band carries it, and nothing else on screen claims
  // to know the Mac's audio.
  bool muted = false;

  // One short word. No mark distinguishes YouTube from IINA from a blind
  // scrub, and the seek buttons mean different things under each.
  const char* profileName = "";

  // What the Mac says is playing, from the unlock helper. Empty when nothing
  // is, or when no helper is there to say. Drawn in the status row only when
  // that row has nothing more urgent to carry: a pairing instruction or an
  // unlock refusal is something to act on, and a song title is not.
  const char* nowTitle = "";
  const char* nowArtist = "";

  UnlockFace unlockFace = UnlockFace::Ask;

  // A challenge is in flight. The button's own band carries it, because a
  // four-second wait with nothing on screen is a button that did nothing.
  bool unlockBusy = false;
};

void buildRemote(toybox::Screen& screen, const RemoteModel& model);

// The confirm in front of clearing the bonds. Destructive in the sense that
// matters here: the Mac has to be told to forget the device too, and until it
// is the pair will not re-form.
struct ForgetModel {
  const char* detail = "";
};

void buildForgetConfirm(toybox::Screen& screen, const ForgetModel& model);

// --- Unlock ----------------------------------------------------------------

// The PIN pad. Digits only and no letters, because the PIN's whole job is to
// be typed on a touchscreen in a hurry -- and because what it unseals has no
// verifier, so its strength comes from the Mac counting wrong answers rather
// than from its own length.
struct PinModel {
  const char* title = "PIN";
  // One line under the title. Carries the last refusal when there was one:
  // a wrong PIN is indistinguishable from an unpaired Mac until the Mac says
  // so, and the screen has to say which it was told.
  const char* detail = "";
  uint8_t entered = 0;  // how many digits so far; drawn as marks, never as digits
  bool canConfirm = false;
};

void buildPin(toybox::Screen& screen, const PinModel& model);

// The pairing code, shown once. Eight groups of four, because thirty-two
// unbroken characters is a line nobody types correctly.
struct PairModel {
  const char* code = "";  // 32 symbols, no separators; this screen groups them
  const char* detail = "";
};

void buildPair(toybox::Screen& screen, const PairModel& model);

}  // namespace remoteui
