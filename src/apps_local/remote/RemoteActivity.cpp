#include "RemoteActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "../../components/UITheme.h"
#include "../Shelf.h"
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
// How long Spotlight is given to open before the name is typed, and to search
// before Return commits. Both are generous: the cost of being early is that
// Return lands on the wrong application, and the cost of being late is a third
// of a second.
constexpr uint32_t kSpotlightOpenMs = 400;
constexpr uint32_t kSpotlightSettleMs = 600;

// The unlock pairing: a salt, the sealed secret and the replay counter. Its
// own file rather than a line in settings.txt, because it is binary and
// because deleting it is how the pairing is forgotten.
constexpr const char* kPairing = "/.crosspoint/remote/unlock.bin";
constexpr uint8_t kPairingVersion = 1;

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

void RemoteActivity::openClaude() {
  // Spotlight, because it is the only route to an application that needs
  // nothing configured on the Mac first: tap Command-Space, type the name,
  // press Return. A HELD Command-Space is Siri, which is why the two buttons
  // differ only in how long the same chord is held.
  const remote::KeyChord chord = remote::commandSpace();
  remote::Chord out;
  out.modifiers = chord.modifiers;
  out.key = chord.key;
  if (!remote::sendChord(out)) {
    LOG_INF(kTag, "no subscribed host; Claude launch dropped");
    return;
  }
  // Spotlight has to be on screen and focused before the name means anything.
  delay(kSpotlightOpenMs);
  remote::typeText(remote::kClaudeQuery);
  // And it has to have finished searching, or Return commits against a stale
  // top hit -- which on a Mac means opening whatever was there before.
  delay(kSpotlightSettleMs);
  remote::sendReturn();
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
  HalFile file;
  if (!Storage.openFileForRead(kTag, kPairing, file)) return false;
  uint8_t buffer[1 + remote::vault::kSaltLen + remote::vault::kSecretLen + 8] = {};
  const int n = file.read(buffer, sizeof(buffer));
  file.close();
  if (n != static_cast<int>(sizeof(buffer)) || buffer[0] != kPairingVersion) {
    LOG_INF(kTag, "unlock: no usable pairing on the card");
    return false;
  }
  size_t at = 1;
  std::memcpy(salt_, buffer + at, sizeof(salt_));
  at += sizeof(salt_);
  std::memcpy(sealed_, buffer + at, sizeof(sealed_));
  at += sizeof(sealed_);
  counter_ = 0;
  for (size_t i = 0; i < 8; ++i) counter_ = (counter_ << 8) | buffer[at + i];
  return true;
}

void RemoteActivity::savePairing() {
  Storage.ensureDirectoryExists(kDir);
  uint8_t buffer[1 + remote::vault::kSaltLen + remote::vault::kSecretLen + 8] = {};
  buffer[0] = kPairingVersion;
  size_t at = 1;
  std::memcpy(buffer + at, salt_, sizeof(salt_));
  at += sizeof(salt_);
  std::memcpy(buffer + at, sealed_, sizeof(sealed_));
  at += sizeof(sealed_);
  for (size_t i = 0; i < 8; ++i) buffer[at + i] = static_cast<uint8_t>(counter_ >> (56 - i * 8));
  HalFile file;
  if (!Storage.openFileForWrite(kTag, kPairing, file)) {
    LOG_ERR(kTag, "unlock: could not write the pairing");
    return;
  }
  file.write(buffer, sizeof(buffer));
  file.close();
}

void RemoteActivity::clearSecrets() {
  remote::vault::wipe(secret_, sizeof(secret_));
  remote::vault::wipe(freshSecret_, sizeof(freshSecret_));
  remote::vault::wipe(pin_, sizeof(pin_));
  remote::vault::wipe(pairCode_, sizeof(pairCode_));
  haveSecret_ = false;
  pinLen_ = 0;
}

void RemoteActivity::forgetUnlockPairing() {
  clearSecrets();
  Storage.remove(kPairing);
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
  if (!haveSecret_) {
    RenderLock lock(*this);
    pinPurpose_ = PinPurpose::Open;
    pinLen_ = 0;
    pin_[0] = '\0';
    pinDetail_ = "";
    phase_ = Phase::Pin;
    requestUpdate();
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

  // Ask and Unlock both send the same request: the Mac is the one that knows
  // whether its screen is locked, so it answers with the state AND, only if it
  // really is locked, with the password.
  startChallenge(remote::vault::Op::Unlock);
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
  if (!haveSecret_) return false;
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
      if (haveSecret_) startChallenge(remote::vault::Op::Status);
    }
    return;
  }

  const remote::helper::State state = remote::helper::poll();
  if (state == remote::helper::State::TimedOut) {
    RenderLock lock(*this);
    unlockBusy_ = false;
    macScreen_ = remote::vault::Screen::Unknown;
    unlockDetail_ = "The Mac did not answer.";
    remote::helper::cancel();
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
    LOG_INF(kTag, "unlock: refused, verdict %d", static_cast<int>(verdict));
    if (verdict == remote::vault::Verdict::BadMac) {
      // A wrong PIN and an unpaired Mac are the same wall from here -- the
      // sealed secret has no verifier, which is the whole point of it -- so
      // the screen says both and asks for the PIN again.
      clearSecrets();
      RenderLock lock(*this);
      unlockBusy_ = false;
      macScreen_ = remote::vault::Screen::Unknown;
      pinPurpose_ = PinPurpose::Open;
      pinDetail_ = "Wrong PIN, or this Mac no longer knows this reader.";
      phase_ = Phase::Pin;
      requestUpdate();
      return;
    }
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

// --- The PIN pad -----------------------------------------------------------

void RemoteActivity::pinDigit(const int digit) {
  if (pinLen_ >= remote::vault::kPinMaxLen) return;
  RenderLock lock(*this);
  pin_[pinLen_++] = static_cast<char>('0' + digit);
  pin_[pinLen_] = '\0';
  requestUpdate();
}

void RemoteActivity::pinBackspace() {
  if (pinLen_ == 0) return;
  RenderLock lock(*this);
  pin_[--pinLen_] = '\0';
  requestUpdate();
}

void RemoteActivity::pinConfirm() {
  if (!remote::vault::pinIsWellFormed(pin_)) return;

  if (pinPurpose_ == PinPurpose::Choose) {
    remote::helper::randomBytes(salt_, sizeof(salt_));
    remote::vault::sealSecret(freshSecret_, pin_, salt_, sealed_);
    std::memcpy(secret_, freshSecret_, sizeof(secret_));
    remote::vault::wipe(freshSecret_, sizeof(freshSecret_));
    remote::vault::wipe(pairCode_, sizeof(pairCode_));
    counter_ = 0;
    savePairing();
    RenderLock lock(*this);
    paired_ = true;
    haveSecret_ = true;
    pinLen_ = 0;
    remote::vault::wipe(pin_, sizeof(pin_));
    phase_ = Phase::Remote;
    macScreen_ = remote::vault::Screen::Unknown;
    unlockDetail_ = "";
    requestUpdate();
    LOG_INF(kTag, "unlock: paired");
    return;
  }

  // Opening. This always "works" -- a wrong PIN yields a wrong secret and
  // nothing here can tell -- so the next challenge is what finds out.
  remote::vault::openSecret(sealed_, pin_, salt_, secret_);
  {
    RenderLock lock(*this);
    haveSecret_ = true;
    pinLen_ = 0;
    remote::vault::wipe(pin_, sizeof(pin_));
    pinDetail_ = "";
    phase_ = Phase::Remote;
    requestUpdate();
  }
  startChallenge(remote::vault::Op::Unlock);
}

// --- Input ---------------------------------------------------------------

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
  pollChallenge();
  // A track change is the one thing the Mac pushes unprompted. The link
  // reports only CHANGES, so this repaints on a new song and never on a timer.
  remote::NowPlaying next;
  if (remote::helper::takeNowPlaying(next)) {
    RenderLock lock(*this);
    nowPlaying_ = next;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (phase_ != Phase::Remote) {
      // Backing out of the PIN pad or the pairing code abandons whatever it
      // was for; nothing half-entered is kept.
      remote::vault::wipe(pin_, sizeof(pin_));
      remote::vault::wipe(freshSecret_, sizeof(freshSecret_));
      remote::vault::wipe(pairCode_, sizeof(pairCode_));
      RenderLock lock(*this);
      pinLen_ = 0;
      phase_ = Phase::Remote;
      requestUpdate();
      return;
    }
    shelf::leave(renderer, mappedInput);
    return;
  }

  // The two side keys are volume, because that is the control people reach for
  // without looking at the panel and the only one worth a physical button.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    volumeStep(true);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    volumeStep(false);
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
    case remoteui::ActionSiri: {
      // The SAME chord the Claude button taps, held. That is how macOS itself
      // tells Siri from Spotlight, and it is why neither button needs a
      // setting: both ride a shortcut a stock Mac already has.
      const remote::KeyChord chord = remote::commandSpace();
      remote::Chord out;
      out.modifiers = chord.modifiers;
      out.key = chord.key;
      remote::holdChord(out, remote::kSiriHoldMs);
      requestUpdate();
      break;
    }
    case remoteui::ActionClaude:
      openClaude();
      break;
    case remoteui::ActionUnlock:
      tapUnlock();
      break;
    case remoteui::ActionPinDigit:
      pinDigit(event.value);
      break;
    case remoteui::ActionPinBack:
      pinBackspace();
      break;
    case remoteui::ActionPinOk:
      pinConfirm();
      break;
    case remoteui::ActionPairDone: {
      // The code has been typed into the Mac. Now the PIN that seals it here.
      RenderLock lock(*this);
      pinPurpose_ = PinPurpose::Choose;
      pinLen_ = 0;
      pin_[0] = '\0';
      pinDetail_ = "This PIN unseals the pairing. There is no way to recover it.";
      phase_ = Phase::Pin;
      requestUpdate();
      break;
    }
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
  } else if (phase_ == Phase::Pin) {
    remoteui::PinModel model;
    model.title = pinPurpose_ == PinPurpose::Choose ? "SET PIN" : "PIN";
    model.detail = pinDetail_;
    model.entered = pinLen_;
    model.canConfirm = remote::vault::pinIsWellFormed(pin_);
    remoteui::buildPin(screen, model);
    what = "Remote PIN";
  } else if (phase_ == Phase::Pair) {
    remoteui::PairModel model;
    model.code = pairCode_;
    model.detail = "Type this into the unlock helper on the Mac. It is shown once.";
    remoteui::buildPair(screen, model);
    what = "Remote pair";
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
    remoteui::buildRemote(screen, model);
  }

  interactionsReady_ = true;
  toybox::reportOverflow(interactions_, what);
  const bool phaseChanged = !everShown_ || phase_ != lastShownPhase_;

  const auto labels = mappedInput.mapLabels("Back", "", "Vol+", "Vol-");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  if (phaseChanged) {
    lastShownPhase_ = phase_;
    phaseShownAtMs_ = millis();
    everShown_ = true;
  }
}
