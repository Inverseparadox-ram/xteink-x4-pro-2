#pragma once

// The unlock button's own GATT service, alongside the HID one.
//
// ---------------------------------------------------------------------------
// Why a second service exists at all.
//
// HID is one-way. Reports go out; nothing comes back. That is fine for every
// other button on the panel -- a volume step needs no answer -- but the unlock
// button must not type a password until the Mac has proved two things: that it
// holds the paired secret, and that its screen is actually locked. Neither
// fact can travel over a HID input report, so this service carries them.
//
// Two characteristics and nothing else:
//
//   CHALLENGE  notify   the reader's 58-byte request
//   RESPONSE   write    the Mac's answer, up to 139 bytes
//
// Both directions are authenticated by RemoteVault, so the service itself can
// stay as dumb as it looks: it moves bytes and reports whether any arrived.
// Every decision about whether to believe them is made in one place, and that
// place is testable on a host.
//
// The Mac side is a LaunchAgent; docs/apps/remote.md carries it in full. It
// finds this service on the peripheral macOS has already connected for HID --
// a CoreBluetooth central may talk GATT to a system-connected peripheral --
// which is why nothing here has to manage a second connection.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

#include "RemoteVault.h"

namespace remote {
// Not `link`: RemoteHid.h already has a function of that name in this
// namespace, and a namespace and a function cannot share one.
namespace helper {

// Where the exchange has got to. Drawn on the button, so every value here is
// a thing the screen has to be able to say.
enum class State : uint8_t {
  Idle,      // nothing asked
  Waiting,   // asked, no answer yet
  Answered,  // an answer arrived and is waiting to be taken
  TimedOut,  // nobody answered in time -- usually the helper is not running
};

// How long the Mac gets. Generous: the helper has to be woken by CoreBluetooth,
// read the lock state and reach the Keychain, and it is doing that on a machine
// whose screen is off.
inline constexpr uint32_t kAnswerTimeoutMs = 4000;

// Adds the service to the server. Called from remote::begin() BEFORE the GATT
// server is started: NimBLE registers services at start, and one created after
// that is a service the host never sees. There is exactly one radio lifecycle
// and this hangs off it.
void begin();
void end();

// For the advertisement RemoteHid builds. The scan response carries it rather
// than the advertisement: a 128-bit UUID is eighteen bytes and thirty-one are
// already spoken for by the flags, the HID UUID and the appearance.
const char* serviceUuid();

// True once a host has subscribed to CHALLENGE -- which is the only evidence
// the reader gets that a helper is running at all. Advertised and connected is
// not the same thing: macOS connects for HID whether or not anything is
// listening for this.
bool helperPresent();

// Sends `challenge` and starts the clock. False when there is nobody
// subscribed, which the screen reports rather than waiting four seconds to
// find out.
bool ask(const vault::Challenge& challenge);

// Call from the activity loop; moves Waiting to TimedOut when the clock runs
// out.
State poll();

// Takes the answer, clearing it. False unless state() is Answered.
bool take(vault::Response& out);

// Abandons whatever is in flight.
void cancel();

// The hardware entropy source, wrapped here so the activity need not include
// an ESP header to build a nonce. A nonce drawn from millis() would repeat
// across a reboot and repeat the keystream with it.
void randomBytes(uint8_t* out, size_t len);

}  // namespace helper
}  // namespace remote
