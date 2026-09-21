# The flash budget

What happens when the app slot fills up, and what to cut first.

## Where it stands

`check.sh` prints this on every run:

```
x4pro   Flash: [========= ]  87.0% (used 7241342 bytes from 8323072 bytes)
```

The slot is `app0` in [partitions.csv](../partitions.csv): **8,323,072 bytes
(7.94MB)**. At CrossPlay 1.13.9 plus Clock, Weather and Remote that leaves
**1,081,730 bytes free** -- about 1.03MB.

For scale: the three fork apps together are roughly 120KB of that. The move
from upstream 1.13.2 to 1.13.9 cost 1.6 points on its own. **The pressure comes
from upstream, not from adding apps**, which is worth knowing before cutting
anything of your own.

## What happens if it fills

**The build fails. The device is never at risk.**

Two independent checks stand in front of it, and both run before anything
reaches the panel:

1. PlatformIO's `checkprogsize` compares the linked image against the partition
   size and fails the build -- the same step that prints the percentage above.
2. `scripts_local/verify-image.py` check 3, "firmware.bin fits app0", refuses
   the merged image.

There is no path where an oversized image gets written to the device, so this
cannot brick anything. You simply cannot produce a `-full.bin` that does not
fit. The failure is a red build, not a dead reader.

## What is actually in there

Measured on the 1.13.9 release image with `xtensa-esp-elf-nm --size-sort -S`:

| Block | Bytes | Share |
| --- | ---: | ---: |
| **Fonts** (Noto Serif + Noto Sans + Ubuntu, 4 sizes x 4 styles, plus the big Toybox cuts) | 2,230,479 | 33% |
| **i18n strings** (34 languages) | 597,139 | 9% |
| **Hyphenation tries** (10 languages) | 351,667 | 5% |
| **Icons** | 139,612 | 2% |
| **Web UI pages** | 65,109 | 1% |
| Everything else -- code, the EPUB engine, NimBLE, wolfSSL, the apps | 3,384,265 | 50% |

Half the image is not code. That is what makes it cuttable.

## Workarounds, largest first

### 1. Trim the languages -- about 900KB, no feature loss for one reader

34 translations and 10 hyphenation dictionaries, of which one person uses one.
Together that is **948,806 bytes, 11.4% of the slot** -- more than eight times
the whole of Clock, Weather and Remote.

Delete the `lib/I18n/translations/*.yaml` you do not want (English is the
reference and must stay) and the unused
`lib/Epub/Epub/hyphenation/generated/hyph-*.trie.h`. Both are regenerated from
source, so the build picks the change up on its own.

The one real cost: an upstream sync will keep re-adding them, so this is a
deletion you re-apply on each `smerge`.

### 2. Drop a font family -- about 1MB

`src/main.cpp` loads Noto Serif and Noto Sans, each at 12/14/16/18pt in four
styles. If you only ever read in one of them, the other is roughly a megabyte
doing nothing. `OMIT_FONTS` already exists in `main.cpp` as an all-or-nothing
switch; a per-family version is a few lines in the same block.

This one is visible: it removes a choice from the reader's font setting.

### 3. Remove apps you do not use

Cheaper than it sounds and rarely worth it -- the shelf's 21 games are mostly
tens of KB each, because the expensive things (fonts, packs, wallpapers) are
shared or live on the SD card. Delete the directory, its `kApps`/`kGames` row,
its icon and its tests. Budget maybe 20-80KB apiece.

### 4. Single app slot -- doubles the budget, and costs OTA

The nuclear option, and the only one that changes the ceiling rather than the
contents.

`partitions.csv` gives two 7.94MB app slots. **The second one is not for crash
rollback** -- a running app cannot erase the flash it is executing from, so an
over-the-air update needs somewhere else to land. Collapsing `app0` and `app1`
into one partition yields roughly **15.9MB, about double**.

What it costs:

- **No OTA, ever.** Every update becomes a USB `write_flash`. If that is
  already how you update -- it is how every `-full.bin` in `dist/` is
  installed -- the loss is theoretical.
- `host-tests/release` asserts the two app slots stay the same size and that
  the old spiffs partition stays gone. That guard would need rewriting, not
  suppressing.
- The table only changes on a full serial flash, which `write_flash 0x0` does.

Do not reach for this first. Upstream already fought the same fight and won
2.6MB by reclaiming a spiffs partition nothing mounted -- the comment at the top
of `partitions.csv` is worth reading before adding a fourth idea to the list.

## If you want the numbers again

```bash
pio run -e gh_release_x4pro
NM=/root/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp-elf-nm
$NM --size-sort -S --radix=d .pio/build/gh_release_x4pro/firmware.elf | tail -40
```

The tail is the biggest symbols; font bitmaps and hyphenation tries dominate it.
