#include "RemoteActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#if defined(CROSSPLAY_BLE_HID)
#include <Preferences.h>
#endif

#include <HalClock.h>

#include <cstdio>
#include <cstring>

#include "../../components/UITheme.h"
#include "../Shelf.h"
#include "../clock/ClockCore.h"
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxTheme.h"

namespace fui = freeink::ui;

namespace {
constexpr const char* kTag = "REMOTE";
constexpr const char* kDir = "/.crosspoint/remote";
constexpr const char* kSettings = "/.crosspoint/remote/settings.txt";
// How often the battery level is pushed to the host. Once a minute: it is the
// one fact a HID peripheral can report back, and it changes slowly.
constexpr uint32_t kBatteryIntervalMs = 60000;

// The unlock pairing -- the shared secret and the replay counter -- lives in
// the reader's INTERNAL flash, not on the SD card. Nothing seals it (the
// padlock unlocks on one tap, by the owner's choice), so the card, which is
// removed and read in other machines to load books, is the wrong place for it:
// a copy of the secret is enough to ask the Mac for its password. Taking it
// out of internal flash needs the reader, a cable and flashing tools.
//
// Preferences, as DeviceReport.cpp keeps its own device secret. A USB full
// flash erases this along with the Bluetooth bond, so both pair again; the SD
// card updater leaves both alone.
constexpr const char* kPairingSpace = "rcunlock";
constexpr const char* kPairingSecretKey = "secret";
constexpr const char* kPairingCounterKey = "counter";

// Where earlier builds kept the pairing, sealed under a combination. Deleted
// on sight: it is useless without that combination and is exactly the thing
// that should not be on the card.
constexpr const char* kLegacyPairing = "/.crosspoint/remote/unlock.bin";

// The simulator has no internal flash to speak of, so it keeps the pairing on
// its pretend card instead. Never built for a device.
constexpr const char* kSimPairing = "/.crosspoint/remote/unlock-sim.bin";

// How long the login window is given to come up and take focus after the
// display is woken. Too early and the password lands on a black screen that
// was still fading in; the cost of being late is a second.
constexpr uint32_t kWakeSettleMs = 1400;

// How long after doing something that should have changed the Mac before
// asking it what actually happened. Long enough for the login window to have
// accepted a password and for the helper to see the session open.
constexpr uint32_t kStatusFollowUpMs = 2500;
}  // namespace

std::unique_ptr<Activity> RemoteActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return makeUniqueNoThrow<RemoteActivity>(renderer, mappedInput);
}

// --- Settings ------------------------------------------------------------

void RemoteActivity::loadSettings() {
  HalFile file;
  if (!Storage.openFileForRead(kTag, kSettings, file)) return;
  char buffer[64] = {};
  const int n = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer) - 1);
  file.close();
  if (n <= 0) return;
  int profile = 0;
  // "profile=%d" and nothing else. Files written before the volume slider was
  // removed carry a trailing "volume=N"; sscanf stops at the profile and the
  // rest is ignored, so an old file still restores the one setting left.
  if (std::sscanf(buffer, "profile=%d", &profile) == 1) {
    if (profile >= 0 && profile < static_cast<int>(remote::Profile::Count)) {
      profile_ = static_cast<remote::Profile>(profile);
    }
  }
}

void RemoteActivity::saveSettings() {
  Storage.ensureDirectoryExists(kDir);
  char text[64];
  const int n = std::snprintf(text, sizeof(text), "profile=%d\n", static_cast<int>(profile_));
  if (n <= 0) return;
  HalFile file;
  if (!Storage.openFileForWrite(kTag, kSettings, file)) return;
  file.write(reinterpret_cast<const uint8_t*>(text), static_cast<size_t>(n));
  file.close();
}

// --- Lifecycle -----------------------------------------------------------

void RemoteActivity::onEnter() {
  Activity::onEnter();
  toybox::ensureFonts(renderer);
  loadSettings();
  paired_ = loadPairing();
  // remote::begin() brings up the unlock service with the rest of the radio.
  remote::begin();
  lastLink_ = remote::link();
  LOG_INF(kTag, "opened; advertising as '%s'", remote::deviceName());
  requestUpdate();
}

void RemoteActivity::onExit() {
  saveSettings();
  // Before the radio goes, and before anything else: the opened secret and
  // whatever PIN opened it live in RAM only, and this is where that ends.
  clearSecrets();
  // The radio comes down with the app. A remote is the one app here with a
  // standing reason to hold one, and holding it after the user has walked away
  // is how a device with a month of battery becomes one with a weekend.
  remote::end();
  Activity::onExit();
}

// --- Sending -------------------------------------------------------------

void RemoteActivity::press(const remote::Key key) {
  if (!remote::send(key)) {
    // Not an error worth a screen: the band already says whether anything is
    // connected, and the honest answer to a tap with no host is that nothing
    // happened.
    LOG_INF(kTag, "no subscribed host; key dropped");
  }
  requestUpdate();
}

void RemoteActivity::seek(const bool forward) {
  const remote::KeyChord chord = forward ? remote::forwardChord(profile_) : remote::backChord(profile_);
  if (chord.key != 0) {
    remote::Chord out;
    out.modifiers = chord.modifiers;
    out.key = chord.key;
    remote::sendChord(out);
  } else {
    // No shortcut for this profile: hold the transport's own scrub key, which
    // is the only thing that moves a player this app knows nothing about. Held
    // rather than tapped because a tap of fast-forward is ignored by every
    // host that treats it as scrub -- which is all of them.
    remote::hold(forward ? remote::Key::FastForward : remote::Key::Rewind, remote::kScrubHoldMs);
  }
  requestUpdate();
}

void RemoteActivity::volumeStep(const bool up) {
  if (!remote::send(up ? remote::Key::VolumeUp : remote::Key::VolumeDown)) {
    LOG_INF(kTag, "no subscribed host; volume step dropped");
    return;
  }
  // Moving the volume is an answer to "is it muted": it is not, any more. The
  // Mac unmutes itself on a volume key, so the button follows it rather than
  // keeping a mark the host has already cleared.
  if (muted_) {
    RenderLock lock(*this);
    muted_ = false;
    requestUpdate();
  }
}

void RemoteActivity::toggleMute() {
  if (!remote::send(remote::Key::Mute)) {
    LOG_INF(kTag, "no subscribed host; mute dropped");
    return;
  }
  RenderLock lock(*this);
  // The remote cannot read the Mac's mute, so this tracks what IT sent. Mute
  // is a toggle on the host too, so the two agree unless somebody mutes on the
  // Mac directly.
  muted_ = !muted_;
  requestUpdate();
}

void RemoteActivity::cycleProfile() {
  {
    RenderLock lock(*this);
    profile_ =
        static_cast<remote::Profile>((static_cast<int>(profile_) + 1) % static_cast<int>(remote::Profile::Count));
  }
  saveSettings();
  requestUpdate();
}

// --- Unlock ---------------------------------------------------------------
//
// The whole point of what follows is that the reader does not decide to type.
// It asks, the Mac answers under a key only the paired Mac holds, and the
// answer says both "it is me" and "my screen is locked". RemoteVault.h has the
// argument in full; this is the part that drives it.

bool RemoteActivity::loadPairing() {
  counter_ = 0;
#if defined(CROSSPLAY_BLE_HID)
  if (Storage.exists(kLegacyPairing)) {
    Storage.remove(kLegacyPairing);
    LOG_INF(kTag, "unlock: removed an old sealed pairing from the card");
  }
  Preferences prefs;
  // Read-only, and false when the namespace has never been written: that is
  // simply "not paired".
  if (!prefs.begin(kPairingSpace, true)) return false;
  const bool ok = prefs.getBytesLength(kPairingSecretKey) == sizeof(secret_) &&
                  prefs.getBytes(kPairingSecretKey, secret_, sizeof(secret_)) == sizeof(secret_);
  counter_ = prefs.getULong64(kPairingCounterKey, 0);
  prefs.end();
  return ok;
#else
  HalFile file;
  if (!Storage.openFileForRead(kTag, kSimPairing, file)) return false;
  uint8_t buffer[remote::vault::kSecretLen + 8] = {};
  const int n = file.read(buffer, sizeof(buffer));
  file.close();
  if (n != static_cast<int>(sizeof(buffer))) return false;
  std::memcpy(secret_, buffer, sizeof(secret_));
  for (size_t i = 0; i < 8; ++i) counter_ = (counter_ << 8) | buffer[sizeof(secret_) + i];
  return true;
#endif
}

void RemoteActivity::savePairing() {
#if defined(CROSSPLAY_BLE_HID)
  Preferences prefs;
  if (!prefs.begin(kPairingSpace, false)) {
    LOG_ERR(kTag, "unlock: could not open internal storage for the pairing");
    return;
  }
  prefs.putBytes(kPairingSecretKey, secret_, sizeof(secret_));
  prefs.putULong64(kPairingCounterKey, counter_);
  prefs.end();
#else
  Storage.ensureDirectoryExists(kDir);
  uint8_t buffer[remote::vault::kSecretLen + 8] = {};
  std::memcpy(buffer, secret_, sizeof(secret_));
  for (size_t i = 0; i < 8; ++i) buffer[sizeof(secret_) + i] = static_cast<uint8_t>(counter_ >> (56 - i * 8));
  HalFile file;
  if (!Storage.openFileForWrite(kTag, kSimPairing, file)) return;
  file.write(buffer, sizeof(buffer));
  file.close();
#endif
}

void RemoteActivity::clearSecrets() {
  remote::vault::wipe(secret_, sizeof(secret_));
  remote::vault::wipe(freshSecret_, sizeof(freshSecret_));
  remote::vault::wipe(pairCode_, sizeof(pairCode_));
}

void RemoteActivity::forgetUnlockPairing() {
  clearSecrets();
#if defined(CROSSPLAY_BLE_HID)
  Preferences prefs;
  if (prefs.begin(kPairingSpace, false)) {
    prefs.clear();
    prefs.end();
  }
#else
  Storage.remove(kSimPairing);
#endif
  RenderLock lock(*this);
  paired_ = false;
  counter_ = 0;
  macScreen_ = remote::vault::Screen::Unknown;
  unlockDetail_ = "";
  LOG_INF(kTag, "unlock: pairing forgotten");
}

void RemoteActivity::beginPairing() {
  remote::helper::randomBytes(freshSecret_, sizeof(freshSecret_));
  remote::vault::encodeSecret(freshSecret_, pairCode_, sizeof(pairCode_));
  RenderLock lock(*this);
  phase_ = Phase::Pair;
  requestUpdate();
}

void RemoteActivity::tapUnlock() {
  if (unlockBusy_) return;

  if (!paired_) {
    beginPairing();
    return;
  }

  // An unlocked Mac is locked with a plain keyboard chord and no exchange at
  // all. Locking is not a security decision -- the worst a forged one can do
  // is lock a screen -- so it must keep working when the helper is asleep,
  // which is exactly when somebody wants it.
  if (remote::vault::intentFor(macScreen_) == remote::vault::Intent::Lock) {
    sendLockChord();
    return;
  }

  // One tap. The Mac is the one that knows whether its screen is locked, so
  // it answers with the state AND, only if it really is locked, the password.
  startChallenge(remote::vault::Op::Unlock);
}

void RemoteActivity::finishPairing() {
  RenderLock lock(*this);
  std::memcpy(secret_, freshSecret_, sizeof(secret_));
  remote::vault::wipe(freshSecret_, sizeof(freshSecret_));
  remote::vault::wipe(pairCode_, sizeof(pairCode_));
  counter_ = 0;
  savePairing();
  paired_ = true;
  phase_ = Phase::Remote;
  macScreen_ = remote::vault::Screen::Unknown;
  unlockDetail_ = "";
  // Ask straight away, so the padlock shows the Mac's real state rather than
  // a question.
  statusDueAt_ = millis() + kStatusFollowUpMs;
  requestUpdate();
  LOG_INF(kTag, "unlock: paired");
}

void RemoteActivity::sendLockChord() {
  const remote::vault::LockChord chord = remote::vault::lockChord();
  remote::Chord out;
  out.modifiers = chord.modifiers;
  out.key = chord.key;
  if (!remote::sendChord(out)) {
    RenderLock lock(*this);
    unlockDetail_ = "Nothing is connected.";
    requestUpdate();
    return;
  }
  RenderLock lock(*this);
  // The reader has SENT a lock, which is not the same as knowing one happened
  // -- an application can swallow the chord. So the face goes back to the
  // question and the follow-up below finds out for real.
  macScreen_ = remote::vault::Screen::Unknown;
  unlockDetail_ = "";
  statusDueAt_ = millis() + kStatusFollowUpMs;
  requestUpdate();
}

bool RemoteActivity::startChallenge(const remote::vault::Op op) {
  if (!paired_) return false;
  if (!remote::helper::helperPresent()) {
    RenderLock lock(*this);
    // The distinction matters and no mark carries it: the Mac is connected as
    // a keyboard -- every other button works -- and the helper that answers
    // this one is not running.
    unlockDetail_ = "The Mac is connected, but the unlock helper is not running.";
    macScreen_ = remote::vault::Screen::Unknown;
    requestUpdate();
    return false;
  }

  remote::vault::Challenge challenge;
  challenge.op = op;
  // Bumped and PERSISTED before it goes out, so a reader that reboots mid
  // exchange never reuses a number the Mac has already accepted.
  ++counter_;
  challenge.counter = counter_;
  savePairing();
  remote::helper::randomBytes(challenge.nonce, sizeof(challenge.nonce));
  remote::vault::signChallenge(secret_, sizeof(secret_), challenge);

  if (!remote::helper::ask(challenge)) {
    RenderLock lock(*this);
    unlockDetail_ = "The Mac is not listening.";
    requestUpdate();
    return false;
  }
  RenderLock lock(*this);
  pending_ = challenge;
  unlockBusy_ = true;
  unlockDetail_ = "";
  requestUpdate();
  return true;
}

void RemoteActivity::pollChallenge() {
  if (!unlockBusy_) {
    if (statusDueAt_ != 0 && millis() >= statusDueAt_) {
      statusDueAt_ = 0;
      // Quietly: a status check nobody asked for must not put "the helper is
      // not running" on the panel, only a tap of the padlock should.
      if (paired_ && remote::helper::helperPresent()) startChallenge(remote::vault::Op::Status);
    }
    return;
  }

  const remote::helper::State state = remote::helper::poll();
  if (state == remote::helper::State::TimedOut) {
    // The helper answers nothing it cannot verify, so silence means it is not
    // running, is paused or locked out after wrong answers, or no longer holds
    // this reader's pairing. `crossplay-unlock status` on the Mac says which.
    remote::helper::cancel();
    RenderLock lock(*this);
    unlockBusy_ = false;
    macScreen_ = remote::vault::Screen::Unknown;
    unlockDetail_ = "The Mac did not answer.";
    requestUpdate();
    return;
  }
  if (state != remote::helper::State::Answered) return;

  remote::vault::Response response;
  if (!remote::helper::take(response)) {
    RenderLock lock(*this);
    unlockBusy_ = false;
    unlockDetail_ = "The Mac answered with something that is not an answer.";
    requestUpdate();
    return;
  }
  finishUnlock(response);
}

void RemoteActivity::finishUnlock(const remote::vault::Response& response) {
  const remote::vault::Verdict verdict = remote::vault::verifyResponse(secret_, sizeof(secret_), pending_, response);
  if (verdict != remote::vault::Verdict::Ok) {
    // Not the paired Mac, or not an answer to this question. Nothing it says
    // is believed, and nothing is typed.
    LOG_INF(kTag, "unlock: refused, verdict %d", static_cast<int>(verdict));
    RenderLock lock(*this);
    unlockBusy_ = false;
    macScreen_ = remote::vault::Screen::Unknown;
    unlockDetail_ = "The Mac's answer did not check out.";
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    unlockBusy_ = false;
    macScreen_ = response.screen;
    unlockDetail_ = "";
    requestUpdate();
  }

  if (pending_.op != remote::vault::Op::Unlock || response.screen != remote::vault::Screen::Locked) return;

  // Verified, locked, and the answer carried a password. On the stack and
  // wiped before this function returns: it is the one thing in this app that
  // must not outlive the keystrokes.
  char password[remote::vault::kMaxSecretPayload + 1] = {};
  const size_t length =
      remote::vault::openPayload(secret_, sizeof(secret_), pending_, response, password, sizeof(password));
  if (length == 0) {
    RenderLock lock(*this);
    unlockDetail_ = "The Mac is locked but sent no password.";
    requestUpdate();
    return;
  }

  // The display is probably asleep, and a password typed at a screen that has
  // not finished waking is a failed attempt the Mac counts.
  remote::wakeHost();
  delay(kWakeSettleMs);
  const bool typed = remote::typeSecret(password);
  if (typed) remote::sendReturn();
  remote::vault::wipe(password, sizeof(password));

  RenderLock lock(*this);
  if (!typed) unlockDetail_ = "That password has a character this keyboard cannot type.";
  // Sent, not known to have worked. The follow-up below asks the Mac.
  macScreen_ = remote::vault::Screen::Unknown;
  statusDueAt_ = millis() + kStatusFollowUpMs;
  requestUpdate();
}

// --- F8 and the microphones --------------------------------------------------

void RemoteActivity::toggleHeldKey() {
  if (remote::keyHeld()) {
    remote::releaseHeld();
  } else if (!remote::pressHeld(remote::kHoldKeyUsage)) {
    LOG_INF(kTag, "no subscribed host; F8 not held");
    return;
  }
  requestUpdate();
}

void RemoteActivity::toggleMicrophones() {
  // Only an unmute needs a known state to justify it. Anything unknown asks
  // for the safe direction.
  const bool unmute = macState_.known && macState_.microphonesMuted;
  const remote::MacCommand command =
      unmute ? remote::MacCommand::UnmuteMicrophones : remote::MacCommand::MuteMicrophones;
  if (!remote::helper::sendCommand(command)) {
    RenderLock lock(*this);
    unlockDetail_ = "The Mac is connected, but the unlock helper is not running.";
    requestUpdate();
    return;
  }
  // No local flip: the band follows what the Mac REPORTS, which arrives on its
  // own a moment later. A band flipped here would claim a mute that the Mac
  // might have failed to apply.
}

// --- Input ---------------------------------------------------------------

// Callers hold the render lock: render() reads what this sets.
void RemoteActivity::askForBoard(const int board) {
  if (board < 0 || board > 1) return;
  pullWanted_[board] = true;
  pullSentAt_[board] = 0;
}

// Sends the pulls that are due, and notices their answers and their silence.
void RemoteActivity::pullBoards() {
  static constexpr uint32_t kAnswerMs = 8000;
  static constexpr remote::StatusBoardId kIds[2] = {remote::StatusBoardId::Claude, remote::StatusBoardId::Services};
  const remote::StatusAssembler& boards = remote::helper::statusBoards();
  for (int b = 0; b < 2; ++b) {
    const bool onScreen = phase_ == Phase::Remote && page_ == b + 1;
    if (pullWanted_[b] && remote::helper::sendPull(kIds[b])) {
      RenderLock lock(*this);
      pullWanted_[b] = false;
      pullSentAt_[b] = millis();
      if (pullSentAt_[b] == 0) pullSentAt_[b] = 1;
    }
    const uint16_t arrived = boards.arrivals(kIds[b]);
    if (arrived != boardsSeen_[b]) {
      // Any complete board is an answer: the pull's, or a restart's progress.
      RenderLock lock(*this);
      boardsSeen_[b] = arrived;
      pullSentAt_[b] = 0;
      struct tm now = {};
      if (halClock.localTime(now) && now.tm_year >= 120) {
        clockapp::Civil civil;
        civil.hour = static_cast<uint8_t>(now.tm_hour);
        civil.minute = static_cast<uint8_t>(now.tm_min);
        char clock[12];
        clockapp::formatClock(civil, clock, sizeof(clock));
        std::snprintf(boardStamp_[b], sizeof(boardStamp_[b]), "Updated %s", clock);
      } else {
        std::snprintf(boardStamp_[b], sizeof(boardStamp_[b]), "Updated just now");
      }
      if (onScreen) requestUpdate();
    } else if (pullSentAt_[b] != 0 && millis() - pullSentAt_[b] > kAnswerMs) {
      RenderLock lock(*this);
      pullSentAt_[b] = 0;
      std::snprintf(boardStamp_[b], sizeof(boardStamp_[b]), "The Mac did not answer");
      if (onScreen) requestUpdate();
    }
  }
}

void RemoteActivity::loop() {
  // A connection appearing or dropping changes what the band says and whether
  // the pairing sentence is on screen, and nothing else will repaint it.
  const remote::Link now = remote::link();
  if (now != lastLink_) {
    lastLink_ = now;
    requestUpdate();
  }
  if (remote::ready() && millis() - lastBatteryAt_ > kBatteryIntervalMs) {
    lastBatteryAt_ = millis();
    remote::setBattery(static_cast<uint8_t>(powerManager.getBatteryPercentage()));
  }
  // When the helper appears -- the app just opened, or the Mac reconnected --
  // ask it straight away, so the padlock shows the Mac's real state.
  const bool helperNow = remote::helper::helperPresent();
  if (helperNow && !helperWasPresent_ && paired_ && !unlockBusy_) statusDueAt_ = millis() + 300;
  // A list on screen when the helper (re)appears asks for itself again.
  if (helperNow && !helperWasPresent_ && page_ != 0) {
    RenderLock lock(*this);
    askForBoard(page_ - 1);
  }
  helperWasPresent_ = helperNow;
  pollChallenge();
  // A track change is the one thing the Mac pushes unprompted. The link
  // reports only CHANGES, so this repaints on a new song and never on a timer.
  remote::NowPlaying next;
  if (remote::helper::takeNowPlaying(next)) {
    RenderLock lock(*this);
    nowPlaying_ = next;
    requestUpdate();
  }
  // The time in the status row changes once a minute, and nothing else would
  // repaint it. Only while it is actually on screen: a song hides it.
  if (phase_ == Phase::Remote && remote::link() == remote::Link::Connected && !nowPlaying_.present()) {
    struct tm now = {};
    if (halClock.localTime(now)) {
      const int minuteOfDay = now.tm_hour * 60 + now.tm_min;
      if (minuteOfDay != lastClockMinute_) {
        lastClockMinute_ = minuteOfDay;
        requestUpdate();
      }
    }
  }
  // Pages 2 and 3. Drained on every page so nothing piles up, repainted only
  // when a list is on screen: the controls page shows none of it.
  if (remote::helper::takeStatus() && phase_ == Phase::Remote && page_ != 0) requestUpdate();
  pullBoards();
  remote::MacState macNext;
  if (remote::helper::takeMacState(macNext)) {
    RenderLock lock(*this);
    macState_ = macNext;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (phase_ != Phase::Remote) {
      // Backing out of the pairing code abandons it: the Mac may have been
      // given the code, but this reader keeps nothing of it.
      remote::vault::wipe(freshSecret_, sizeof(freshSecret_));
      remote::vault::wipe(pairCode_, sizeof(pairCode_));
      RenderLock lock(*this);
      phase_ = Phase::Remote;
      requestUpdate();
      return;
    }
    shelf::leave(renderer, mappedInput);
    return;
  }

  // Physically LEFT is logical Up and physically RIGHT is Down (docs/buttons.md).
  // On the panel they are volume, the control reached for without looking. On
  // the pairing code either one moves on, because touch has been unreliable
  // enough that the one screen asking for a confirmation takes a key too.
  const bool up = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool down = !up && mappedInput.wasReleased(MappedInputManager::Button::Down);
  if (up || down) {
    if (phase_ == Phase::Remote) {
      volumeStep(up);
    } else if (phase_ == Phase::Pair) {
      finishPairing();
    }
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (!mappedInput.wasScreenTapped(tapX, tapY) || !interactionsReady_) return;
  if (everShown_ && millis() - phaseShownAtMs_ < kSettleMs) return;

  fui::InputSnapshot input;
  input.touchReleased = true;
  input.touchX = static_cast<int16_t>(tapX);
  input.touchY = static_cast<int16_t>(tapY);
  const fui::ActionEvent event = interactions_.route(input);

  switch (event.action) {
    case remoteui::ActionPlayPause:
      press(remote::Key::PlayPause);
      break;
    case remoteui::ActionNext:
      press(remote::Key::Next);
      break;
    case remoteui::ActionPrevious:
      press(remote::Key::Previous);
      break;
    case remoteui::ActionHoldKey:
      toggleHeldKey();
      break;
    case remoteui::ActionMicrophone:
      toggleMicrophones();
      break;
    case remoteui::ActionUnlock:
      tapUnlock();
      break;
    case remoteui::ActionPairDone:
      finishPairing();
      break;
    case remoteui::ActionForward:
      seek(true);
      break;
    case remoteui::ActionBack:
      seek(false);
      break;
    case remoteui::ActionVolumeUp:
      volumeStep(true);
      break;
    case remoteui::ActionVolumeDown:
      volumeStep(false);
      break;
    case remoteui::ActionMute:
      toggleMute();
      break;
    case remoteui::ActionProfile:
      cycleProfile();
      break;
    case remoteui::ActionRestartRow: {
      const remote::StatusBoard& board = remote::helper::statusBoards().board(remote::StatusBoardId::Services);
      if (event.value < 0 || event.value >= board.count) break;
      const remote::StatusRow& row = board.rows[event.value];
      RenderLock lock(*this);
      restartRow_ = event.value;
      std::snprintf(restartTitle_, sizeof(restartTitle_), "%s", row.title);
      std::snprintf(restartDetail_, sizeof(restartDetail_), "%s: %s", remote::statusWord(row.status), row.detail);
      phase_ = Phase::Restart;
      requestUpdate();
      break;
    }
    case remoteui::ActionRestartConfirm: {
      if (restartRow_ >= 0 && !remote::helper::sendRestart(static_cast<uint8_t>(restartRow_), restartTitle_)) {
        LOG_ERR("REMOTE", "restart: the helper is not listening");
      }
      RenderLock lock(*this);
      restartRow_ = -1;
      phase_ = Phase::Remote;
      // The Mac marks the row before it answers, so this pull reads
      // "restarting"; what happens after is REFRESH's to fetch.
      askForBoard(1);
      requestUpdate();
      break;
    }
    case remoteui::ActionRestartCancel: {
      RenderLock lock(*this);
      restartRow_ = -1;
      phase_ = Phase::Remote;
      requestUpdate();
      break;
    }
    case remoteui::ActionNextPage: {
      RenderLock lock(*this);
      page_ = (page_ + 1) % remoteui::kPageCount;
      if (page_ != 0) askForBoard(page_ - 1);
      requestUpdate();
      break;
    }
    case remoteui::ActionRefreshBoard:
      if (page_ != 0) {
        RenderLock lock(*this);
        askForBoard(page_ - 1);
        requestUpdate();
      }
      break;
    case remoteui::ActionForget: {
      RenderLock lock(*this);
      phase_ = Phase::Forget;
      requestUpdate();
      break;
    }
    case remoteui::ActionForgetConfirm:
      remote::forgetPairings();
      // And the unlock secret with it. Leaving it behind would hand the next
      // Mac to pair a reader that still holds the last one's key.
      forgetUnlockPairing();
      {
        RenderLock lock(*this);
        phase_ = Phase::Remote;
      }
      // Advertising has to come back up: deleting the bonds drops the host.
      remote::end();
      remote::begin();
      requestUpdate();
      break;
    case remoteui::ActionForgetCancel: {
      RenderLock lock(*this);
      phase_ = Phase::Remote;
      requestUpdate();
      break;
    }
    default:
      break;
  }
}

// --- Drawing -------------------------------------------------------------

void RemoteActivity::render(RenderLock&&) {
  renderer.clearScreen();
  fui::GfxRendererTarget target = toybox::makeTarget(renderer, toybox::readingFaces());
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  interactionsReady_ = false;
  toybox::Frame frame(target, device, noInput, interactions_);
  toybox::Screen screen(frame);

  const char* what = "Remote";

  if (phase_ == Phase::Forget) {
    remoteui::ForgetModel model;
    model.detail =
        "This clears the pairing on the device, and the unlock secret with it. "
        "You also have to remove CrossPlay Remote in the Mac's Bluetooth "
        "settings, or it will refuse to pair again.";
    remoteui::buildForgetConfirm(screen, model);
    what = "Remote unpair";
  } else if (phase_ == Phase::Pair) {
    remoteui::PairModel model;
    model.code = pairCode_;
    model.detail = "Type this into the unlock helper on the Mac. It is shown once. Then press either side key.";
    remoteui::buildPair(screen, model);
    what = "Remote pair";
  } else if (phase_ == Phase::Restart) {
    remoteui::RestartModel model;
    model.title = restartTitle_;
    model.detail = restartDetail_;
    remoteui::buildRestartConfirm(screen, model);
    what = "Remote restart";
  } else if (page_ != 0) {
    const bool claude = page_ == 1;
    remoteui::StatusPageModel model;
    model.title = claude ? "CLAUDE" : "MAC";
    model.page = page_;
    model.board =
        &remote::helper::statusBoards().board(claude ? remote::StatusBoardId::Claude : remote::StatusBoardId::Services);
    model.helperConnected = remote::link() == remote::Link::Connected && remote::helper::helperPresent();
    model.emptyLine = claude ? "No Claude Code sessions on the Mac. If there should be, run "
                               "crossplay-unlock claude-setup there once."
                             : "No services listed. Edit services.txt beside the helper on the Mac.";
    model.offerForget = !claude;
    model.restartable = !claude;
    model.stamp = boardStamp_[page_ - 1];
    model.refreshing = pullWanted_[page_ - 1] || pullSentAt_[page_ - 1] != 0;
    remoteui::buildStatusPage(screen, model);
    what = claude ? "Remote claude" : "Remote mac";
  } else {
    const remote::Link link = remote::link();
    remoteui::RemoteModel model;
    model.connected = link == remote::Link::Connected;
    if (!model.connected) {
      // Named steps, because a BLE peripheral that is merely "not connected"
      // tells nobody what to do next.
      // The only sentence left in the app, and it earns its place: no mark
      // says "System Settings > Bluetooth", and a remote the Mac cannot see
      // has to say where to look.
      pairingHint_ = std::string("On the Mac: System Settings > Bluetooth, then \"") + remote::deviceName() +
                     "\". Discoverable only while this screen is open.";
      model.pairingHint = pairingHint_.c_str();
    } else if (unlockDetail_[0] != '\0') {
      // Connected, but the unlock button has something to report that its own
      // mark cannot carry.
      model.pairingHint = unlockDetail_;
    }
    if (nowPlaying_.present()) {
      model.nowTitle = nowPlaying_.title;
      model.nowArtist = nowPlaying_.artist;
    } else {
      // Read the way the Clock app reads it, and formatted by the same
      // function, so the two never disagree about what time it is.
      struct tm now = {};
      if (halClock.localTime(now) && now.tm_year >= 120) {
        clockapp::Civil civil;
        civil.hour = static_cast<uint8_t>(now.tm_hour);
        civil.minute = static_cast<uint8_t>(now.tm_min);
        clockapp::formatClock(civil, clockText_, sizeof(clockText_));
        model.clockText = clockText_;
      }
      model.batteryPercent = static_cast<int8_t>(powerManager.getBatteryPercentage());
    }
    model.forwardSeconds = remote::forwardSeconds(profile_);
    model.backSeconds = remote::backSeconds(profile_);
    model.muted = muted_;
    model.profileName = remote::profileName(profile_);
    switch (remote::vault::intentFor(macScreen_)) {
      case remote::vault::Intent::Unlock:
        model.unlockFace = remoteui::UnlockFace::Unlock;
        break;
      case remote::vault::Intent::Lock:
        model.unlockFace = remoteui::UnlockFace::Lock;
        break;
      case remote::vault::Intent::Ask:
      default:
        model.unlockFace = remoteui::UnlockFace::Ask;
        break;
    }
    model.unlockBusy = unlockBusy_;
    model.keyHeld = remote::keyHeld();
    model.micKnown = macState_.known;
    model.micMuted = macState_.microphonesMuted;
    remoteui::buildRemote(screen, model);
  }

  interactionsReady_ = true;
  toybox::reportOverflow(interactions_, what);
  const bool phaseChanged = !everShown_ || phase_ != lastShownPhase_ || page_ != lastShownPage_;

  const auto labels = mappedInput.mapLabels("Back", "", "Vol+", "Vol-");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  if (phaseChanged) {
    lastShownPhase_ = phase_;
    lastShownPage_ = page_;
    phaseShownAtMs_ = millis();
    everShown_ = true;
  }
}
