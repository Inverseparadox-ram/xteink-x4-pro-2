# Remote

A Bluetooth media remote for whatever the Mac is already playing.

## Why it is a remote and not a player

Because the hardware cannot be a player, and that is worth stating once with
the evidence rather than being rediscovered.

**No Bluetooth Classic.** Playing to a Bluetooth speaker means A2DP, which
needs BR/EDR. The ESP32-S3's `soc_caps.h` defines `SOC_BLE_SUPPORTED` and no
classic-BT capability at all, and Espressif ships `esp_a2dp_api.h` for the
original ESP32 and for no other chip in this toolchain.

**No audio output either.** `BoardConfig.h` sets
`FREEINK_CAP_AUDIO (FREEINK_DEVICE_MURPHY || FREEINK_DEVICE_M5)` and
`FREEINK_CAP_BUZZER` to four boards that do not include this one. The X4 Pro
has no codec, no DAC and not even a beeper.

So the device does not carry the music. It carries the **controls**: paired as
a BLE HID keyboard, it sends the key presses a media keyboard sends, and the
Mac plays on to whatever speaker it was already using.

## NimBLE, not Bluedroid

The prebuilt Arduino libs for this target are `CONFIG_BT_NIMBLE_ENABLED`, with
no Bluedroid. That is why `platformio.ini`'s `[base]` carries
`lib_ignore = BLE` -- the Arduino `BLE` library is Bluedroid-based and cannot
link here -- and why this app uses `NimBLE-Arduino` and `NimBLEHIDDevice`.

The stack itself is already inside those prebuilt libs, so adding the whole
radio cost about **46KB of flash**: 81.9% to 82.4%.

## What it cannot do, and how the screen handles it

**HID is one-way.** Reports go out; nothing comes back. The remote cannot know
the track, the artist, the volume, or whether anything is playing.

So no screen here shows a state it cannot know:

- **Volume is two buttons, not a slider.** HID sends volume *steps*, not
  levels, and cannot read one back, so a slider drew a position the remote had
  guessed at -- and the guess was wrong the moment anyone touched the volume on
  the Mac. Two buttons claim nothing: one tap is one step, which is exactly
  what goes over the wire. The side keys do the same thing without looking at
  the panel.
- **The mute button remembers only what it sent.** It cannot read the Mac's
  mute, so the filled band means "this remote sent a mute", not "the Mac is
  muted". A volume step clears it, because a volume key unmutes on the host
  too.

**There is no now-playing display, and there cannot be one over BLE from a
Mac.** iOS publishes AMS (Apple Media Service), a BLE service that would answer
exactly those questions; macOS does not expose it. Showing metadata would mean
a helper running on the Mac pushing it over Wi-Fi, which is a different app.

## Seek, and why it is a keystroke

**There is no HID usage meaning "skip ten seconds."** The transport has Fast
Forward and Rewind, and on every host that implements them they are
scrub-while-held, not a jump. The ten and five second jumps people mean belong
to the *player*.

So the seek buttons type the player's own shortcut, and a profile says which.
The footer names the profile, and the button faces change with it: the
circular arrow carries a **number only where the profile can keep one**. Under
the others it stands alone, because a button reading "10" that scrubs for as
long as the host feels like would be lying.

| Profile | Forward | Back | Seek buttons show |
| --- | --- | --- | --- |
| `YOUTUBE` | `L` | `←` | arrow + `10` / arrow + `5` -- exact, YouTube defines them |
| `PLAYER KEYS` | `→` | `←` | the arrows alone -- the player sets the jump |
| `SCRUB` | hold Fast Forward | hold Rewind | the arrows alone -- the host sets the distance |

## The three shortcut buttons

The third row is a microphone, Claude's mark and a padlock. None of the three
is a media key -- HID has no usage for Siri and none for launching an
application -- so the first two type a keyboard shortcut, the same way the seek
buttons do. The third needed something HID cannot do at all; see below.

| Button | Sends | Needs setting up? |
| --- | --- | --- |
| Siri | `⌘Space` **held** for 1.2s | No, if Siri's shortcut is "Hold ⌘ Space" (System Settings → Apple Intelligence & Siri) |
| Claude | `⌘Space` tapped, then types `claude`, then Return | No -- that is Spotlight, and it is stock |
| Unlock / Lock | A challenge, then the password; or `⌃⌘Q` | **Yes**, once. See below |

**Siri and Claude ride the same chord.** macOS itself separates Siri from
Spotlight by whether ⌘Space is *held* or *tapped*, so this remote does too --
which is why neither button needs anything configured that a Mac does not
already have. `host-tests/remote` asserts the two stay on one chord.

This row replaced explicit PLAY, PAUSE and STOP buttons. Those were the
transport toggle spelled out three times; the toggle above is what macOS
honours most reliably anyway, so they cost a third of the panel and added one
control it did not already give. The padlock replaced a Do Not Disturb button
that sent `⌃⌥⌘D`, a chord with no default binding anywhere in macOS -- so it
did nothing at all until the user made a Shortcut for it, and the remote had no
way to tell whether they had.

## The unlock button

**macOS exposes no API for dismissing the lock screen.** Not to a signed
helper, not to a LaunchAgent, not to anything: Apple reserves unlocking for the
password field, Touch ID and Apple Watch. So the only way any Bluetooth device
unlocks a Mac is to **type the password**, and that is what this does.

Given that the keystrokes *are* the unlock, the exchange in front of them buys
four things, and each is a failure it removes:

1. **The reader holds no password.** It lives in the Mac's Keychain. The Mac
   sends it, encrypted under a key derived from the paired secret and a nonce
   that has never been used before, and only in answer to a request it has
   authenticated.
2. **The reader will not type into a stranger's Mac**, because it has nothing
   to type until a host holding the secret has answered.
3. **The reader will not type into an unlocked session.** The Mac reports its
   real lock state inside the MAC, and sends no password unless the screen is
   locked. Without this the failure is ugly and silent: press unlock at an
   awake Mac and the password goes into whatever field has focus.
4. **A stolen reader is not a key.** The secret is sealed under a PIN, and no
   verifier is stored beside it -- every PIN opens the blob into a well-formed
   secret, so there is nothing to test a guess against offline. The only oracle
   is the Mac, which counts wrong answers and backs off.

What it does **not** buy, stated plainly: anyone with the reader *and* the PIN
can unlock the Mac. That is the design -- a key and a code -- not a gap in it.
And it does nothing at the FileVault pre-boot screen, because Bluetooth is not
up that early.

### The face is what the Mac last said

| Face | Means | A tap |
| --- | --- | --- |
| Question shield | Nothing verified yet | Asks |
| Open padlock | The Mac said it is locked | Sends the password |
| Closed padlock | The Mac said it is awake | Sends `⌃⌘Q` |

It is read from the last **verified** answer and never from what the reader
last did. A reader that assumed the Mac was still locked because it had locked
it would be wrong the first time anyone touched the Mac's own keyboard -- and
being wrong here means typing a password into an open session.

**Locking needs no helper at all.** It is a plain `⌃⌘Q`, because locking a
screen is not a security decision -- the worst a forged one can do is lock a
screen -- and a lock button that stops working when the helper is asleep is
broken exactly when someone wants it.

### The wire

A second GATT service beside the HID one, because HID is one-way and this needs
an answer. Two characteristics, both fixed-shape:

| | |
| --- | --- |
| service | `6F1B0A00-9D3C-4F5E-8A77-2B4C1D6E9F01` |
| challenge (notify) | 58 bytes: version, op, counter, 16-byte nonce, MAC |
| response (write) | 11-byte head, the sealed password, 32-byte MAC |

Three keys, one per purpose, all `HMAC(secret, label ‖ nonce)`, so the key that
authenticates a request can never be made to produce a keystream. The password
is encrypted with HMAC-SHA256 in counter mode and then MAC'd -- encrypt-then-
MAC, so a bent payload is rejected before anything decrypts it. The counter is
persisted on both sides and only ever goes up, which is what stops a recorded
unlock request being replayed at a locked Mac.

`src/apps_local/remote/RemoteVault.h` is the definition, and
`host-tests/remotevault` proves it against **FIPS 180-4, RFC 4231 and RFC
7914** -- 612 checks, including every single-bit flip of the MAC and of the
ciphertext. That is why SHA-256 is reimplemented there rather than called from
the ESP-IDF: a MAC nobody can run on a host is a MAC nobody can prove.

### The Mac half

`tools_local/remote-mac/` -- a Swift LaunchAgent, its build script and its
launchd plist, with the install steps in its own README. It keeps running
behind the lock screen, which is the whole reason it is an agent.

### Setting it up

1. On the reader, open **Remote** and press the padlock. With nothing paired it
   shows a 32-character code in eight groups of four, **once**.
2. On the Mac, `crossplay-unlock pair`, and type that code and the login
   password.
3. Back on the reader, press TYPED IT and choose a PIN of 4 to 12 digits. There
   is no recovery: forget it and you re-pair.

The PIN is asked once per time the app is opened, and the opened secret lives
in RAM only -- `onExit` wipes it along with the radio.

### Keyboard layout

HID carries key *positions*, not characters, and the Mac decides what each
position produces. The reader types a **US layout**. On any other layout the
letters land but the symbols do not. Nothing comes back over HID, so the
symptom is a password that silently fails, and there is nothing the reader can
do about it.

## The screen is marks, not words

Every control is an icon: 64px prev / play-pause / next across the top, the two
circular seek arrows, then the three shortcuts at 40px, then a speaker mark
with volume down, volume up and mute. Three pieces of text survive the whole
panel, and each one is there because no drawing does its job:

- the **two seek numbers**, under the one profile that defines them;
- the **profile name** in the footer, because no mark distinguishes YouTube
  from IINA from a blind scrub while the seek buttons mean different things
  under each;
- the **status sentence**, because nothing draws "System Settings > Bluetooth"
  and nothing draws "the unlock helper is not running" either. Connected with
  nothing to report, it collapses to a single bluetooth glyph -- the live
  controls under it are the rest of the message.

Two screens behind the panel are words by necessity: the **PIN pad**, which
draws how many digits have been typed and never which, and the **pairing
code**, which is a code from another machine and cannot be a picture.

`host-tests/ui` asserts the absence directly: it renders the panel and fails if
`PLAY/PAUSE`, `VOLUME`, `MUTE`, `FWD`, `PREV`, `NEXT`, `SIRI`, `CLAUDE`, `DND`,
`FOCUS`, `UNLOCK` or `LOCK` ever reach it as text. A label creeping back onto a
button face is invisible in a diff and obvious on the device.

It also taps the centre of every control and asserts that control wins the hit
test. Registered and reachable are different questions: a button the router
hands to its neighbour does nothing, for a reason no screenshot shows.

## One report per key, and why the gap matters

A key is held for **45ms** before its release report goes out, and 45ms passes
before the next report.

That number used to be 12ms, and it was a bug. A BLE notification only leaves
the device when its connection interval comes round -- macOS negotiates 15-30ms
-- so two notifications sent 12ms apart could land in the same interval, where
the second `setValue()` overwrote the first before either was transmitted. The
host then saw the release and never the press.

The symptom picked on **mute** specifically, and that is the tell. A drag of
the old volume slider fired sixteen reports, so enough survived for the volume
to visibly move; play/pause was simply pressed again by anyone who thought they
had missed. Mute is one tap carrying one report, and a lost report is a button
that does nothing.

## The radio is up only while the app is open

`onExit` takes it down. A remote is the one app in this fork with a standing
reason to hold a radio, and holding it after the user has walked away is how a
device with a month of battery becomes one with a weekend. The pairing sentence
says as much, because "it is only discoverable while this screen is open" is
the first thing someone hunting for it in the Mac's Bluetooth list needs.

The one thing a HID peripheral *can* report back is its battery, pushed once a
minute, which is why macOS shows a level beside the device.

## Pairing

The device advertises as **CrossPlay Remote**, an Apple-vendor HID keyboard
(`0x05AC`) so macOS treats it as a media keyboard it already understands rather
than an unknown peripheral -- that PnP id is the difference between the media
keys working and being ignored.

Bonding is on, MITM off: macOS pairs a HID peripheral without a passkey prompt
this way, and the bond is what lets it reconnect by itself. The band's unlink
button clears the bonds **and the unlock secret with them** -- leaving that
behind would hand the next Mac to pair a reader that still held the last one's
key. The Mac has to be told to forget the device too, which the confirm screen
says.
