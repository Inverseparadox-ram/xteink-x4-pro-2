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

**HID alone can never show what is playing.** iOS publishes AMS (Apple Media
Service), a BLE service that would answer exactly that; macOS does not expose
it. What changed is that the unlock button put an agent on the Mac with a GATT
channel back to the reader -- see **Now playing** below.

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

## The third row: F8, the microphones, the padlock

Three different kinds of button, which is why they sit together: none of them
is a media key.

| Button | Does | Needs the helper? |
| --- | --- | --- |
| **Pen** | One tap presses F8 and leaves it down; the next lets it go | No -- it is a plain key |
| **Microphone** | Mutes every microphone the Mac has; tap again to unmute | Yes |
| **Padlock** | Unlocks on one tap, or locks with `⌃⌘Q` | Yes, to unlock |

### F8, held

A HID keyboard report is the whole state of the keyboard, not an event, so a
report that leaves a held key out tells the Mac it was let go. Every keyboard
report the remote sends -- seek, the lock chord, the unlock typing -- therefore
carries F8 while it is held, and the key stays down across them. The band is
filled while it is held: that is the one state here the remote owns outright,
because the remote is the thing holding the key. Leaving the app lets it go
before the radio comes down.

**If holding F8 plays or pauses music instead,** macOS is treating it as the
media key printed on an Apple keyboard's F8. The remote identifies as an Apple
keyboard (that is what makes the media keys work), so this depends on System
Settings > Keyboard > "Use F1, F2, etc. keys as standard function keys".

### The microphones

The helper mutes every input device the Mac has, through CoreAudio, device by
device: the mute control where a device has one, otherwise its input volume
set to zero with the old level remembered for the unmute. A Mac mini has no
built-in microphone, so in practice that is AirPods, USB and display
microphones -- including any plugged in while the mute is on, and any an app
turns back up, which the helper checks every five seconds and turns back down.

The band fills only when the **Mac reports** that every input is muted, never
because the button was tapped: a mute that failed to apply must not look like
one that worked. Until the Mac has said anything the button is outlined, and a
tap then asks for a mute, the safe direction.

The command is protected by the Bluetooth bond and nothing more, deliberately:
it has to work without asking the Mac to verify anything. The worst anything misusing it
could do is change whether the microphones are muted.

**It does not touch the camera.** macOS has no supported way for a program to
switch a camera off; the only system-level switch is a device-management
profile of the kind an employer's IT department installs. The camera is not
silently left out: this is the limit, stated.

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
4. **The secret is not on the SD card.** It lives in the reader's internal
   flash, because a copy of it is enough to ask the Mac for its password, and
   the card is the part that gets taken out and read in other machines.

What it does **not** buy, stated plainly: **the reader is a key.** The padlock
unlocks on one tap with nothing entered on the reader, by the owner's choice,
so anyone holding the reader, near the Mac, while the Mac is logged in and
locked, can unlock it.
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
an answer. Three characteristics:

| | |
| --- | --- |
| service | `6F1B0A00-9D3C-4F5E-8A77-2B4C1D6E9F01` |
| challenge (notify) | 58 bytes: version, op, counter, 16-byte nonce, MAC |
| response (write) | 11-byte head, the sealed password, 32-byte MAC |
| now playing (write, encrypted link) | see **Now playing** |

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

### One tap

Press the padlock and the Mac unlocks: no code on the reader, no second step.
The Mac still decides -- it answers only a request signed with the paired
secret, and sends the password only if its screen really is locked -- but
nothing is asked of the person holding the reader.

**Where the secret lives.** In the reader's internal flash (`Preferences`,
the same store `DeviceReport.cpp` uses for its own device secret), not on the
SD card. Nothing seals it, and a copy of it is enough to ask the Mac for its
password from a fake device, so the removable card that gets read in other
machines is the wrong place. The file earlier builds kept there,
`/.crosspoint/remote/unlock.bin`, is deleted the first time the app opens.

**A USB full flash forgets it,** along with the Bluetooth bond, because both
live in the internal flash a full image writes over. The SD card updater keeps
both. See **Updating without losing the pairing** below.

**When the Mac says nothing.** The helper does not answer a request it cannot
verify, so "The Mac did not answer" means it is not running, it is paused or
locked out after wrong answers, or it holds a different pairing -- the Mac was
paired to another code since. `crossplay-unlock status` on the Mac says which.
The lockout still counts: ten unverifiable requests in a row and the helper
stops answering until `crossplay-unlock unblock`.

**The padlock's face is right as soon as the app opens.** When the helper
subscribes, the reader asks it straight away, so the face is the open or closed
padlock rather than a question.

### Setting it up

1. On the reader, open **Remote** and press the padlock. With nothing paired it
   shows a 32-character code in eight groups of four, **once**.
2. On the Mac, `crossplay-unlock pair`, and type that code and the login
   password.
3. Back on the reader, press either side key, or TYPED IT. That is the whole
   pairing.

**A reader paired under a combination or a touch PIN has to pair again,**
once: its secret was sealed under a code this build no longer asks for, and the
old file is removed rather than read.

### Keyboard layout

HID carries key *positions*, not characters, and the Mac decides what each
position produces. The reader types a **US layout**. On any other layout the
letters land but the symbols do not. Nothing comes back over HID, so the
symptom is a password that silently fails, and there is nothing the reader can
do about it.

## Now playing

The title and artist of whatever Music or Spotify is playing, in the status row
above the transport. It rides the unlock helper's GATT service as a third
characteristic, so it needs the helper running and nothing else set up.

**Where it comes from.** Music and Spotify each broadcast a distributed
notification on every play, pause, stop and track change, with the song in the
payload. The helper listens to those. No polling, and -- unlike asking either
app over AppleScript -- no Automation permission prompt.

**What it cannot see: anything else.** A browser playing YouTube broadcasts
nothing, and the system-wide now-playing API is closed to third-party processes
on current macOS. So under the YouTube seek profile, the row usually stays
empty. That is not a fault.

**It appears on the first change after the helper starts.** The helper knows
only what it has been told since launch, so a song already playing when the
Mac logged in shows up at the next pause, play or track change. Pressing play
on the reader is one.

**It never shows a song it can no longer be told about.** When the helper
unsubscribes or the radio goes down, the reader drops the line. When both
players report something, the one PLAYING wins over the one paused, newest
first -- so pausing Spotify while Music plays leaves Music on screen, which is
what is audible.

**The row never moves the controls.** A long title is cut to one line with an
ellipsis rather than wrapped, and both lines are reserved even for a song with
no artist, so the transport sits in the same place for every song. A pairing
instruction or an unlock refusal takes the row over: those are things to act
on, and a title is not.

The frame is five fields -- version, state, title length, title, artist length,
artist -- at most 64 bytes of UTF-8 each, cut at a character and never through
one. `host-tests/remote` pins the bytes, including a Japanese title whose
64-byte cut would otherwise split its last character. It carries no MAC because
it decides nothing: the worst a forged frame could do is print a wrong song,
and the characteristic requires an encrypted link, which in practice means the
bonded Mac.

## The screen is marks, not words

Every control is an icon: 64px prev / play-pause / next across the top, the two
circular seek arrows, then a pen, the microphone and the padlock at 40px, then
volume down, volume up and mute in the same three columns, so the bottom two
rows read as one grid. Three pieces of text survive the whole panel, and each
one is there because no drawing does its job:

- the **two seek numbers**, under the one profile that defines them;
- the **profile name** in the footer, because no mark distinguishes YouTube
  from IINA from a blind scrub while the seek buttons mean different things
  under each;
- the **status row**, which says the most useful thing it has, in this order:
  how to pair ("System Settings > Bluetooth", which nothing draws); why the
  padlock refused; what the Mac is playing; and otherwise the reader's own time
  and charge, as a clock line over a small battery gauge filled to the real
  level beside the percentage. Song and clock take the same two lines, so the
  controls never move between them, and the clock repaints once a minute
  while it is showing and never otherwise.

The one screen behind the panel that uses words is the **pairing code**, which
is a code from another machine and cannot be a picture.


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

### Updating without losing the pairing

**Install new builds with the SD card updater, not the USB full image.** Both
halves of that sentence matter, and the reason is where the bond lives.

NimBLE keeps Bluetooth bonds in the **NVS partition**, at `0x9000`
(`CONFIG_BT_NIMBLE_NVS_PERSIST`). A `-full.bin` is written at `0x0` and runs
straight through to the application, so it writes `0xFF` over all of NVS on the
way -- checked byte for byte on a real image, all 20,480 bytes of it. The reader
forgets the Mac; the Mac still remembers the reader; each refuses the other
until the pairing is redone in both places. Every USB update did this.

**Settings > System > SD Card Firmware Update** writes `firmware.bin` -- the
application alone -- into the other app slot and flips `otadata`. NVS is never
in range, so the bond survives and the Mac reconnects on its own. It has no
version check, so our builds are fine though they all carry the same number: it
checks the image fits the slot, validates the header, checksum and SHA-256, and
refuses an image for the wrong chip or board.

The unlock pairing is not affected either way: it lives on the SD card, in
`/.crosspoint/remote/unlock.bin`.

A USB full flash is still the answer for a first install, for recovery, and
for any release that changes `partitions.csv` -- the one thing an app-only
update cannot carry. Expect to pair again after one.
