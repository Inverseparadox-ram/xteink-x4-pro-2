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
  ActionHoldKey = 383,
  ActionMicrophone = 384,
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

  // F8 is down on the Mac because this remote pressed it. Unlike the Mac's
  // mute, this IS the remote's own state: it is the one holding the key.
  bool keyHeld = false;

  // What the Mac last said about its microphones. Not known means no helper
  // has said anything, and the button then claims nothing either way.
  bool micKnown = false;
  bool micMuted = false;

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

// The pairing code, shown once. Eight groups of four, because thirty-two
// unbroken characters is a line nobody types correctly.
struct PairModel {
  const char* code = "";  // 32 symbols, no separators; this screen groups them
  const char* detail = "";
};

void buildPair(toybox::Screen& screen, const PairModel& model);

}  // namespace remoteui
