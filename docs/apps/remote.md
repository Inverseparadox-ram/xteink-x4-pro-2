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

- **PLAY and PAUSE are separate buttons**, beside the PLAY/PAUSE toggle. The
  toggle is what macOS honours most reliably, but it is a toggle: when the
  remote and the Mac disagree about what is playing -- which they always might
  -- only a button that means one thing gets you out.
- **The volume slider is relative and says so.** HID sends volume *steps*, not
  levels, and cannot read one back. macOS moves in sixteenths, so the slider
  has seventeen positions and a drag from a to b sends |b-a| presses -- exact,
  as long as the volume is only ever changed from here. Dragging to either end
  sends a full sixteen, which is the one move that lands on a level both sides
  agree about after the Mac has been touched directly.
- **The caption says `VOLUME 8 / 16`**, naming the remote's own count rather
  than implying it knows the Mac's.

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
The footer names the profile and its consequence, and the button faces change
with it -- a button that said "+10s" under a profile that cannot deliver ten
seconds would be lying.

| Profile | Forward | Back | Buttons say |
| --- | --- | --- | --- |
| YouTube / browser | `L` | `←` | `+10s` / `-5s` -- exact, YouTube defines them |
| IINA / QuickTime / VLC | `→` | `←` | `FWD` / `BACK` -- the player sets the jump |
| Any player | hold Fast Forward | hold Rewind | `SCRUB >>` / `<< SCRUB` |

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
