# CrossPlay Unlock (the Mac half)

The reader's unlock button needs something on the Mac to answer it. This is
that something: a small agent that verifies the reader's challenge, checks
whether the screen is really locked, and sends the password back encrypted.

Everything here runs on the Mac. Nothing in this folder is built by the
firmware build, and `check.sh` does not touch it -- it cannot, because Swift,
CoreBluetooth and the macOS SDK are not on the reader's build host. It has
been built and run against a real Mac mini and a real reader; see the log
below for what that looked like.

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
3. **A guess has to come through here.** The reader's secret is sealed under an
   eight-press side-key combination with no verifier stored beside it, so there
   is nothing to test a guess against offline. Every wrong combination shows up
   here as a bad MAC, and is not answered. Five in a row start a pause that
   doubles from 30 seconds; **ten in a row lock the helper out** until you run
   `crossplay-unlock unblock`. The count is kept on disk, so restarting the
   helper does not reset it.

It does nothing at the FileVault pre-boot screen: Bluetooth is not up that
early, so a Mac that has been powered off needs its keyboard.

It also **mutes every microphone** the Mac has when the reader's microphone
button asks: the mute control on devices that have one, the input volume set
to zero on those that do not, and anything that comes back on while the mute
is wanted is turned back down within five seconds. It **cannot touch the
camera**: macOS has no supported way for a program to switch one off.

It also tells the reader **what is playing**: the title and artist from Music
or Spotify, which both broadcast a notification on every change. That needs no
extra permission. It cannot see a browser playing YouTube -- nothing on current
macOS will tell a third-party process about that -- and it learns a song only
at the first play, pause or track change after it starts.

## Install

```sh
./build.sh
sudo mkdir -p /usr/local/bin
sudo rm -f /usr/local/bin/crossplay-unlock
sudo cp crossplay-unlock /usr/local/bin/
crossplay-unlock pair
```

`./build.sh` needs Xcode's command line tools (`xcode-select --install`), and
`pair` asks for the reader's code and then this Mac's password.

No `#` comments on the command lines here, and none anywhere else in this file,
because these get pasted into a shell rather than read. **zsh does not treat
`#` as a comment when it is interactive** -- `interactive_comments` is off by
default -- so a commented `sudo mkdir -p /usr/local/bin` creates directories
called `#`, `Apple` and `Silicon` in whatever folder you are standing in.

The `rm` before the `cp` is not tidiness. macOS caches a program's code
signature against the file itself, so copying a new build over the old one
leaves a cached signature that describes the old bytes, and the kernel kills
the new program the moment it starts: `Killed: 9`, with no other message.
Removing it first makes the copy a new file with nothing cached against it.

`/usr/local/bin` is on the default PATH (`/etc/paths` lists it) but nothing
creates it on a Mac whose Homebrew lives in `/opt/homebrew`, so the copy fails
with "No such file or directory" until you make it. Keep that path:
`com.crossplay.unlock.plist` names the binary there.

Run it once in Terminal before installing the agent:

```sh
crossplay-unlock run
```

The first run is when macOS asks for Bluetooth permission, and an agent started
by launchd has no way to ask. Answer yes, check the log says `listening for
challenges`, then stop it and install the agent:

```sh
cp com.crossplay.unlock.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.crossplay.unlock.plist
tail -f /tmp/crossplay-unlock.log
```

`bootstrap`, not `load`: the old subcommand reports almost every failure as
"Load failed: 5: Input/output error", including a plist that is not there.
To take it back out: `launchctl bootout gui/$(id -u)/com.crossplay.unlock`.

A LaunchAgent keeps running behind the lock screen, which is the whole reason
it is an agent and not a login item.

## Updating

After the first install, one command brings it up to date -- pull, build,
install, restart, and print what the helper reports. From this folder,
wherever you cloned it:

```sh
./update.sh
```

It works from any directory if you give the full path. `mdfind -name
CrossPlayUnlock.swift` finds this folder if you have lost track of it.

It asks for your Mac password once, for the copy into `/usr/local/bin`.

## Pairing

On the reader: open **Remote** and press the padlock. With nothing paired it
shows a 32-character code in eight groups of four, **once**. Type that into
`crossplay-unlock pair`, then press either side key on the reader and enter an
eight-press combination of left and right, twice.

The combination seals the secret on the reader's SD card. There is no recovery:
forget it and you re-pair, which means `crossplay-unlock pair` again with a new
code. A reader paired with a touch PIN, before the combination existed, has to
pair again once.

Re-pairing generates a **new secret** on the reader and starts its counter over
at zero, so `pair` resets the replay ledger here to match. It has to: the old
high-water mark belongs to a conversation that no longer exists, and left in
place it refuses every challenge the new pairing sends until the reader climbs
back past it. The reader has no way to be told that, so the symptom is an
unlock button that verifies fine and never works.

## Commands

| | |
| --- | --- |
| `crossplay-unlock pair` | store the reader's code and this Mac's password |
| `crossplay-unlock password` | store a new password, keeping the pairing and its counter |
| `crossplay-unlock unblock` | lift the lockout after ten wrong combinations, keeping the pairing |
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
| now playing (write) | `6F1B0A03-...` -- up to 132 bytes, Mac to reader; `RemoteCore.h` has the layout |

`host-tests/remotevault` proves that header against RFC 4231, RFC 7914 and
FIPS 180-4. If this agent and the reader ever disagree, one of them has drifted
from it; the vectors say which.

## What a working run looks like

Four lines on startup and two per press. This is a real unlock, end to end:

```text
[2026-09-23T15:20:22Z] crossplay-unlock running
[2026-09-23T15:20:28Z] found the reader among the peripherals macOS already has
[2026-09-23T15:20:28Z] connected
[2026-09-23T15:20:28Z] listening for challenges
[2026-09-23T15:29:12Z] unlock: locked, sending
[2026-09-23T15:29:18Z] status: screen is awake
```

`found the reader among the peripherals macOS already has` is the line worth
knowing: a CoreBluetooth central really can talk GATT to the peripheral macOS
is already holding for HID, so the scan path below it is a fallback that never
had to run.

The second pair is the unlock itself. `unlock: locked, sending` is this agent
deciding the screen is genuinely locked and releasing the password; `status:
screen is awake` six seconds later is the READER asking what happened, and it
is the only confirmation either side gets that the password was accepted. Six
seconds is the expected gap: about 1.4s waking the display, then the
keystrokes, then the reader's 2.5s follow-up delay.

Now playing adds one line per change, and one when the reader reconnects,
because the reader forgets the song whenever its radio goes down:

```text
[2026-09-23T16:02:40Z] now playing: Harvest Moon by Neil Young (playing)
[2026-09-23T16:05:51Z] now playing: Harvest Moon by Neil Young (paused)
```

A line saying `write to 6F1B0A03-... failed` means the reader refused the
frame, which should only happen if the link was not encrypted; the helper
retries on the next change.

`crossplay-unlock status` answers the same question from this side. Its
`counter` is the high-water mark of challenges that VERIFIED -- it is bumped
only after the MAC checks out -- so a number above zero is proof the pairing
and the combination are both right. `wrong` is how many wrong answers in a row
it has seen, and says LOCKED OUT once it reaches ten. `mics` is how many input
devices are muted right now.

The microphone button adds a line per press:

```text
[2026-09-28T09:14:02Z] microphones: muted 2 of 2
[2026-09-28T09:31:40Z] microphones: unmuted
```

`cannot mute <device>` means that device offers neither a mute nor a volume a
program may set; it is the one kind this cannot silence.

## If it does not work

| What you see | What it means |
| --- | --- |
| The padlock stays a question mark | The agent is not running, or Bluetooth permission was denied. `tail /tmp/crossplay-unlock.log` |
| "The Mac is connected, but the unlock helper is not running" | HID is up (every other button works) and nothing has subscribed to the challenge characteristic |
| The reader stays on UNLOCKING however you enter the combination | A wrong combination looks exactly like this, by design. So does a helper that is pausing or LOCKED OUT: `crossplay-unlock status`, and `crossplay-unlock unblock` if it says so. The log says `locked out; ignoring` |
| The microphone button never fills | The Mac has not reported every input muted. `crossplay-unlock status` shows how many are; the log names any it `cannot mute` |
| No song on the reader | Nothing has played, paused or changed track since the helper started, or the player is a browser. Press play |
| `Killed: 9` when running `crossplay-unlock` | A new build was copied over the old one in place. `sudo rm /usr/local/bin/crossplay-unlock`, copy it again, then `launchctl kickstart -k gui/$(id -u)/com.crossplay.unlock`. `update.sh` does this itself |
| The log says "replayed" | The two counters are out of step. Re-pairing used to cause it; `pair` now resets this side and the running helper re-reads it before every request, so a build from before those fixes is the likely reason. Delete `~/Library/Application Support/CrossPlayUnlock/ledger.json`, then `launchctl kickstart -k gui/$(id -u)/com.crossplay.unlock` |
| The password is typed but wrong | Non-US keyboard layout, or the password changed since `pair` -- `crossplay-unlock password` fixes the second without disturbing the pairing |
| `status` says `password: missing` after a password reset | The login Keychain was reset with it, which happens when the password is recovered through an Apple ID rather than changed in System Settings. Re-pair |
