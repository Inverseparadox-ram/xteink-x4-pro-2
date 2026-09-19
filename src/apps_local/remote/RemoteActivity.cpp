#include "RemoteActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

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
  int volume = remote::kVolumeSteps / 2;
  if (std::sscanf(buffer, "profile=%d volume=%d", &profile, &volume) >= 1) {
    if (profile >= 0 && profile < static_cast<int>(remote::Profile::Count)) {
      profile_ = static_cast<remote::Profile>(profile);
    }
    volume_ = remote::clampVolume(volume);
  }
}

void RemoteActivity::saveSettings() {
  Storage.ensureDirectoryExists(kDir);
  char text[64];
  const int n = std::snprintf(text, sizeof(text), "profile=%d volume=%d\n", static_cast<int>(profile_), volume_);
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
  remote::begin();
  lastLink_ = remote::link();
  LOG_INF(kTag, "opened; advertising as '%s'", remote::deviceName());
  requestUpdate();
}

void RemoteActivity::onExit() {
  saveSettings();
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

void RemoteActivity::setVolume(const int position) {
  const int target = remote::clampVolume(position);
  const int steps = remote::volumeStepsBetween(volume_, target);
  const remote::Key key = steps >= 0 ? remote::Key::VolumeUp : remote::Key::VolumeDown;
  const int count = steps >= 0 ? steps : -steps;
  for (int i = 0; i < count; ++i) {
    if (!remote::send(key)) break;
    // The host coalesces reports that arrive faster than it polls, so a burst
    // sent flat out moves the volume by less than it was asked for.
    delay(18);
  }
  {
    RenderLock lock(*this);
    volume_ = target;
    // Moving the slider is an answer to "is it muted": it is not, any more.
    if (count > 0) muted_ = false;
  }
  saveSettings();
  requestUpdate();
}

void RemoteActivity::cycleProfile() {
  {
    RenderLock lock(*this);
    profile_ = static_cast<remote::Profile>((static_cast<int>(profile_) + 1) %
                                            static_cast<int>(remote::Profile::Count));
  }
  saveSettings();
  requestUpdate();
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (phase_ == Phase::Forget) {
      RenderLock lock(*this);
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
    setVolume(volume_ + 1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    setVolume(volume_ - 1);
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
    case remoteui::ActionPlay:
      press(remote::Key::Play);
      break;
    case remoteui::ActionPause:
      press(remote::Key::Pause);
      break;
    case remoteui::ActionStop:
      press(remote::Key::Stop);
      break;
    case remoteui::ActionForward:
      seek(true);
      break;
    case remoteui::ActionBack:
      seek(false);
      break;
    case remoteui::ActionVolume:
      // The slider reports the position it was dragged to.
      setVolume(event.value);
      break;
    case remoteui::ActionMute: {
      remote::send(remote::Key::Mute);
      RenderLock lock(*this);
      // The remote cannot read the Mac's mute, so this tracks what IT sent and
      // the button word follows that. Mute is a toggle on the host too, so the
      // two agree unless somebody mutes on the Mac.
      muted_ = !muted_;
      requestUpdate();
      break;
    }
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
        "This clears the pairing on the device. You also have to remove "
        "CrossPlay Remote in the Mac's Bluetooth settings, or it will refuse "
        "to pair again.";
    remoteui::buildForgetConfirm(screen, model);
    what = "Remote unpair";
  } else {
    const remote::Link link = remote::link();
    remoteui::RemoteModel model;
    model.connected = link == remote::Link::Connected;
    model.linkLabel = model.connected ? "CONNECTED" : "PAIR ME";
    if (!model.connected) {
      // Named steps, because a BLE peripheral that is merely "not connected"
      // tells nobody what to do next.
      pairingHint_ = std::string("On the Mac: System Settings > Bluetooth, then pick \"") +
                     remote::deviceName() + "\". It is only discoverable while this screen is open.";
      model.pairingHint = pairingHint_.c_str();
    }
    model.forwardLabel = remote::forwardLabel(profile_);
    model.backLabel = remote::backLabel(profile_);
    model.volume = volume_;
    model.volumeMax = remote::kVolumeSteps;
    model.muted = muted_;
    model.volumeCaption = remote::formatVolume(volume_, volumeCaption_, sizeof(volumeCaption_));
    model.profileName = remote::profileName(profile_);
    model.profileNote = remote::profileNote(profile_);
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
