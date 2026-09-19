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
  ActionDnd = 385,
  ActionForward = 386,
  ActionBack = 387,
  ActionVolumeUp = 388,
  ActionMute = 389,
  ActionProfile = 390,
  ActionForget = 391,
  ActionForgetConfirm = 392,
  ActionForgetCancel = 393,
  ActionVolumeDown = 394,
};

struct RemoteModel {
  // The link state is drawn as a mark. The only words are the ones no mark can
  // carry: where to look on the Mac when it cannot find the device.
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
};

void buildRemote(toybox::Screen& screen, const RemoteModel& model);

// The confirm in front of clearing the bonds. Destructive in the sense that
// matters here: the Mac has to be told to forget the device too, and until it
// is the pair will not re-form.
struct ForgetModel {
  const char* detail = "";
};

void buildForgetConfirm(toybox::Screen& screen, const ForgetModel& model);

}  // namespace remoteui
