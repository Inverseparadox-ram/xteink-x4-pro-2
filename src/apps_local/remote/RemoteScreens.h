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
// therefore a VERB, not a state: PLAY and PAUSE are separate buttons rather
// than one that claims to know which is needed, and the volume caption names
// the remote's own count rather than the Mac's level.
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
  ActionPlay = 383,
  ActionPause = 384,
  ActionStop = 385,
  ActionForward = 386,
  ActionBack = 387,
  ActionVolume = 388,
  ActionMute = 389,
  ActionProfile = 390,
  ActionForget = 391,
  ActionForgetConfirm = 392,
  ActionForgetCancel = 393,
};

struct RemoteModel {
  // "CONNECTED", "PAIR ME" -- what the band says on the right.
  const char* linkLabel = "";
  // The sentence under the transport when nothing is connected. Empty once a
  // host is there, because a connected remote needs no instructions.
  const char* pairingHint = "";
  bool connected = false;

  const char* forwardLabel = "FWD";
  const char* backLabel = "BACK";
  const char* volumeCaption = "";
  int volume = 0;
  int volumeMax = 16;
  bool muted = false;

  // The seek profile's name and its one-line consequence, on the footer row.
  const char* profileName = "";
  const char* profileNote = "";
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
