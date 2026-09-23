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
  enum class Phase : uint8_t { Remote, Forget, Pin, Pair };

  // What the PIN being typed is for. The same twelve keys either unseal a
  // pairing that exists or set the one that is about to.
  enum class PinPurpose : uint8_t { Open, Choose };

  void press(remote::Key key);
  void seek(bool forward);
  void volumeStep(bool up);
  void toggleMute();
  void openClaude();
  void cycleProfile();

  // --- Unlock ---------------------------------------------------------------

  void tapUnlock();
  void beginPairing();
  void pinDigit(int digit);
  void pinBackspace();
  void pinConfirm();
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
  // `sealed_` is what is on the card; `secret_` is what a PIN opened it into
  // and exists in RAM only, for as long as the app is open. Nothing here is
  // ever written out in the clear.
  bool paired_ = false;
  uint8_t salt_[remote::vault::kSaltLen] = {};
  uint8_t sealed_[remote::vault::kSecretLen] = {};
  uint64_t counter_ = 0;

  bool haveSecret_ = false;
  uint8_t secret_[remote::vault::kSecretLen] = {};

  // Held only between the PAIR screen and the PIN that seals it.
  uint8_t freshSecret_[remote::vault::kSecretLen] = {};
  char pairCode_[33] = {};

  char pin_[remote::vault::kPinMaxLen + 1] = {};
  uint8_t pinLen_ = 0;
  PinPurpose pinPurpose_ = PinPurpose::Open;
  const char* pinDetail_ = "";

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
