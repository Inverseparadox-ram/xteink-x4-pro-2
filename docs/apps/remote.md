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

The third row is a microphone, Claude's mark and a crescent moon. None of the
three is a media key -- HID has no usage for Siri, for launching an
application, or for Do Not Disturb -- so each one types a keyboard shortcut,
the same way the seek buttons do.

| Button | Sends | Needs setting up? |
| --- | --- | --- |
| Siri | `⌘Space` **held** for 1.2s | No, if Siri's shortcut is "Hold ⌘ Space" (System Settings → Apple Intelligence & Siri) |
| Claude | `⌘Space` tapped, then types `claude`, then Return | No -- that is Spotlight, and it is stock |
| Do Not Disturb | `⌃⌥⌘D` | **Yes**, once. See below |

**Siri and Claude ride the same chord.** macOS itself separates Siri from
Spotlight by whether ⌘Space is *held* or *tapped*, so this remote does too --
which is why neither button needs anything configured that a Mac does not
already have. `host-tests/remote` asserts the two stay on one chord.

**Do Not Disturb has no default shortcut anywhere in macOS**, so `⌃⌥⌘D` does
nothing until it is bound once:

> Shortcuts app → **+** → search "Set Focus" → set it to *Do Not Disturb,
> Toggle* → rename the shortcut → **ⓘ** → *Add Keyboard Shortcut* → press
> ⌃⌥⌘D.

That chord was picked because nothing in macOS or the common applications
claims it, so binding it takes nothing away. The remote has no way to tell
whether you have bound it -- nothing comes back -- so a Do Not Disturb button
that appears to do nothing means the binding is missing.

This row replaced explicit PLAY, PAUSE and STOP buttons. Those were the
transport toggle spelled out three times; the toggle above is what macOS
honours most reliably anyway, so they cost a third of the panel and added one
control it did not already give.

## The screen is marks, not words

Every control is an icon: 64px prev / play-pause / next across the top, the two
circular seek arrows, then the three shortcuts at 40px, then a speaker mark
with volume down, volume up and mute. Three pieces of text survive the whole
panel, and each one is there because no drawing does its job:

- the **two seek numbers**, under the one profile that defines them;
- the **profile name** in the footer, because no mark distinguishes YouTube
  from IINA from a blind scrub while the seek buttons mean different things
  under each;
- the **pairing sentence**, shown only while unpaired, because nothing draws
  "System Settings > Bluetooth". Once connected it collapses to a single
  bluetooth glyph -- the live controls under it are the rest of the message.

`host-tests/ui` asserts the absence directly: it renders the panel and fails if
`PLAY/PAUSE`, `VOLUME`, `MUTE`, `FWD`, `PREV`, `NEXT`, `SIRI`, `CLAUDE`, `DND`
or `FOCUS` ever reach it as text. A label creeping back onto a button face is
invisible in a diff and obvious on the device.

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
button clears the bonds; the Mac has to be told to forget the device too, which
the confirm screen says.
