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
#include "RemoteLink.h"
#include "RemoteScreens.h"
#include "RemoteVault.h"

class RemoteActivity final : public Activity {
 public:
  RemoteActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Remote", renderer, mappedInput) {}
  ~RemoteActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase : uint8_t { Remote, Forget, Pair };

  void press(remote::Key key);
  void seek(bool forward);
  void volumeStep(bool up);
  void toggleMute();
  void toggleHeldKey();
  void toggleMicrophones();
  void cycleProfile();

  // --- Unlock ---------------------------------------------------------------

  void tapUnlock();
  void beginPairing();
  void finishPairing();
  bool startChallenge(remote::vault::Op op);
  void pollChallenge();
  void finishUnlock(const remote::vault::Response& response);
  void sendLockChord();
  void forgetUnlockPairing();
  void clearSecrets();

  bool loadPairing();
  void savePairing();

  // The store is one line in a file; a whole PersistableStore for a single
  // enum would be more machinery than the setting deserves.
  void loadSettings();
  void saveSettings();

  remote::Profile profile_ = remote::Profile::Browser;
  bool muted_ = false;
  Phase phase_ = Phase::Remote;

  // --- Unlock state ---------------------------------------------------------
  //
  // The shared secret and the replay counter, loaded from internal flash when
  // the app opens and wiped from RAM when it closes. No Mac password is ever
  // here: the Mac sends it, sealed, only at the moment of unlocking.
  bool paired_ = false;
  uint8_t secret_[remote::vault::kSecretLen] = {};
  uint64_t counter_ = 0;

  // Held only while the pairing code is on screen, until a key or TYPED IT
  // makes it the pairing.
  uint8_t freshSecret_[remote::vault::kSecretLen] = {};
  char pairCode_[33] = {};

  // The last VERIFIED answer, and nothing else. A reader that remembered what
  // it had done rather than what it had been told would be wrong the first
  // time anyone touched the Mac's own keyboard -- and being wrong here means
  // typing a password into an open session.
  remote::vault::Screen macScreen_ = remote::vault::Screen::Unknown;
  remote::vault::Challenge pending_;
  bool unlockBusy_ = false;
  const char* unlockDetail_ = "";

  // When to ask the Mac what happened, after doing something that should have
  // changed it. Zero means nothing is due.
  uint32_t statusDueAt_ = 0;

  // What the Mac last said is playing. Copied out of the link on a change, so
  // render() reads a value that cannot move under it.
  remote::NowPlaying nowPlaying_;

  // What the Mac last reported about its microphones.
  remote::MacState macState_;

  // Whether the helper was subscribed at the last loop, so its arrival --
  // the app opening, the Mac reconnecting -- can trigger a status check.
  bool helperWasPresent_ = false;

  // The status row's clock, and the minute it was last drawn at, so the panel
  // repaints once a minute while it shows the time and never otherwise.
  char clockText_[12] = {};
  int lastClockMinute_ = -1;

  // Last time the link state was drawn, so the screen can follow a connection
  // appearing without repainting e-ink on a timer.
  remote::Link lastLink_ = remote::Link::Off;
  uint32_t lastBatteryAt_ = 0;

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
