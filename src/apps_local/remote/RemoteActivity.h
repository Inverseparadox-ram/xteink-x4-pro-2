#pragma once

// The BLE media remote: the controls for whatever the Mac is playing.
//
// The device does not carry the music -- it cannot; see RemoteHid.h for the
// two reasons. Paired as a BLE keyboard it carries the CONTROLS, and the Mac
// plays on to whatever speaker it is already using.
//
// The radio is up only while this app is open. A remote is the one app here
// with a standing reason to hold a radio, and holding it after the user has
// walked away is how an e-ink device with a month of battery becomes one with
// a weekend.

#include <cstdint>
#include <memory>
#include <string>

#include "../../activities/Activity.h"
#include "../ui/ToyboxScreen.h"
#include "RemoteCore.h"
#include "RemoteHid.h"
#include "RemoteScreens.h"

class RemoteActivity final : public Activity {
 public:
  RemoteActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Remote", renderer, mappedInput) {}
  ~RemoteActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase : uint8_t { Remote, Forget };

  void press(remote::Key key);
  void seek(bool forward);
  void setVolume(int position);
  void cycleProfile();

  // The store is one line in a file; a whole PersistableStore for a single
  // enum would be more machinery than the setting deserves.
  void loadSettings();
  void saveSettings();

  remote::Profile profile_ = remote::Profile::Browser;
  int volume_ = remote::kVolumeSteps / 2;
  bool muted_ = false;
  Phase phase_ = Phase::Remote;

  // Last time the link state was drawn, so the screen can follow a connection
  // appearing without repainting e-ink on a timer.
  remote::Link lastLink_ = remote::Link::Off;
  uint32_t lastBatteryAt_ = 0;

  char volumeCaption_[32] = "";
  std::string pairingHint_;

  toybox::Interactions interactions_;
  bool interactionsReady_ = false;

  // A tap within this window of a screen appearing is answering the previous
  // one. Kept here because UNPAIR sits where a control was a moment earlier.
  static constexpr uint32_t kSettleMs = 600;
  Phase lastShownPhase_ = Phase::Remote;
  uint32_t phaseShownAtMs_ = 0;
  bool everShown_ = false;
};
