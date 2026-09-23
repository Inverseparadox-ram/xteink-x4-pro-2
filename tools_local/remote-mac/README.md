# CrossPlay Unlock (the Mac half)

The reader's unlock button needs something on the Mac to answer it. This is
that something: a small agent that verifies the reader's challenge, checks
whether the screen is really locked, and sends the password back encrypted.

Everything here runs on the Mac. Nothing in this folder is built by the
firmware build, and `check.sh` does not touch it -- it cannot, because Swift,
CoreBluetooth and the macOS SDK are not on the reader's build host.

## What it does, and what it deliberately does not

macOS exposes **no API for dismissing the lock screen**. Not to a signed
helper, not to a LaunchAgent, not to anything: Apple reserves unlocking for the
password field, Touch ID and Apple Watch. So the only way any Bluetooth device
unlocks a Mac is to **type the password**, and that is what the reader does.

Given that, the exchange buys three things:

1. **The reader holds no password.** It lives in this Mac's Keychain. This
   agent sends it -- encrypted under a key derived from the paired secret and a
   nonce that has never been used before -- and only in answer to a request it
   has authenticated.
2. **The reader will not type into an unlocked session.** This agent reports
   the real lock state inside the MAC, and sends no password at all unless the
   screen is locked. Without that, pressing unlock at an awake Mac puts the
   password into whatever field has focus.
3. **A guess has to come through here.** The reader's secret is sealed under a
   PIN with no verifier stored beside it, so there is nothing to test a guess
   against offline. Every wrong PIN shows up here as a bad MAC, and five of
   them start a doubling backoff.

It does nothing at the FileVault pre-boot screen: Bluetooth is not up that
early, so a Mac that has been powered off needs its keyboard.

## Install

```sh
./build.sh                                # needs Xcode's command line tools
sudo cp crossplay-unlock /usr/local/bin/
crossplay-unlock pair                     # asks for the reader's code, then your password
```

Run it once in Terminal before installing the agent:

```sh
crossplay-unlock run
```

The first run is when macOS asks for Bluetooth permission, and an agent started
by launchd has no way to ask. Answer yes, check the log says `listening for
challenges`, then stop it and install the agent:

```sh
cp com.crossplay.unlock.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.crossplay.unlock.plist
tail -f /tmp/crossplay-unlock.log
```

A LaunchAgent keeps running behind the lock screen, which is the whole reason
it is an agent and not a login item.

## Pairing

On the reader: open **Remote** and press the padlock. With nothing paired it
shows a 32-character code in eight groups of four, **once**. Type that into
`crossplay-unlock pair`, then press TYPED IT on the reader and choose a PIN.

The PIN seals the secret on the reader's SD card. There is no recovery: forget
it and you re-pair, which means `crossplay-unlock pair` again with a new code.

## Commands

| | |
| --- | --- |
| `crossplay-unlock pair` | store the reader's code and this Mac's password |
| `crossplay-unlock status` | what it has, and whether the screen is locked right now |
| `crossplay-unlock forget` | delete both from the Keychain |
| `crossplay-unlock run` | serve challenges; what launchd runs |

## Keyboard layout

HID carries key *positions*, not characters, and the Mac decides what each
position produces. The reader types a **US layout**. On any other layout the
letters land but the symbols do not, so a password with punctuation in it will
not go in. There is nothing the reader can do about this -- nothing comes back
over HID -- so the symptom is a password that silently fails.

## The wire

Two characteristics on one service, and every byte is defined by
`src/apps_local/remote/RemoteVault.h` in the firmware:

| | |
| --- | --- |
| service | `6F1B0A00-9D3C-4F5E-8A77-2B4C1D6E9F01` |
| challenge (notify) | `6F1B0A01-...` -- 58 bytes, reader to Mac |
| response (write) | `6F1B0A02-...` -- 11 bytes plus payload plus 32, Mac to reader |

`host-tests/remotevault` proves that header against RFC 4231, RFC 7914 and
FIPS 180-4. If this agent and the reader ever disagree, one of them has drifted
from it; the vectors say which.

## If it does not work

| What you see | What it means |
| --- | --- |
| The padlock stays a question mark | The agent is not running, or Bluetooth permission was denied. `tail /tmp/crossplay-unlock.log` |
| "The Mac is connected, but the unlock helper is not running" | HID is up (every other button works) and nothing has subscribed to the challenge characteristic |
| "Wrong PIN, or this Mac no longer knows this reader" | Exactly those two, and the reader cannot tell them apart -- by design |
| The log says "replayed" | The counters are out of step. Re-pair |
| The password is typed but wrong | Non-US keyboard layout, or the password changed since `pair` |
