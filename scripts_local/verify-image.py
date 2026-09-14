#!/usr/bin/env python3
"""Pre-flash checks on a merged CrossPlay image.

  ./scripts_local/verify-image.py .pio/build/gh_release_x4pro \
      dist/crossplay-<version>-x4pro-full.bin partitions.csv

Run it on the file you are about to hand esptool, after merge-bin and before
`write_flash 0x0`. The release workflow checks three magic numbers at this
point, which prove something image-shaped sits at each offset; these compare
the merged file to the built pieces byte for byte, so a merge that placed a
stale or truncated piece cannot pass.

Everything here answers one question: if this file is written to an Xteink X4
Pro at offset 0, does the device come back up?  The checks are ordered by how
badly each failure ends -- a wrong chip or a truncated bootloader is a device
that will not boot at all, a too-large app is one that boots the old slot.
"""
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

BUILD = Path(sys.argv[1])
FULL = Path(sys.argv[2])
CSV = Path(sys.argv[3])

ESP_IMAGE_MAGIC = 0xE9
CHIP_ID_ESP32S3 = 9
PART_MAGIC = b"\xaa\x50"
BOOTLOADER_OFF, PARTTABLE_OFF, APP_OFF = 0x0, 0x8000, 0x10000

failures, checks = [], 0


def ok(label, detail=""):
    global checks
    checks += 1
    print(f"  ok    {label}{('  ' + detail) if detail else ''}")


def bad(label, detail):
    global checks
    checks += 1
    failures.append(label)
    print(f"  FAIL  {label}\n          {detail}")


def want(cond, label, detail):
    ok(label, detail) if cond else bad(label, detail)


print("\n1. The four artefacts a merge needs are on disk")
parts = {}
for name in ("bootloader.bin", "partitions.bin", "firmware.bin", "firmware.elf"):
    p = BUILD / name
    if p.exists() and p.stat().st_size > 0:
        parts[name] = p.read_bytes() if name.endswith(".bin") else b""
        ok(name, f"{p.stat().st_size:,} bytes")
    else:
        bad(name, "missing or empty after a build that reported success")
if failures:
    print("\nCannot continue without all four.")
    sys.exit(1)

print("\n2. Every piece is an image for THIS chip")
# A cross-chip flash is the documented way people have bricked these: the X4
# and X3 are ESP32-C3 and the X4 Pro is S3, and the two images look alike.
for name in ("bootloader.bin", "firmware.bin"):
    blob = parts[name]
    want(blob[0] == ESP_IMAGE_MAGIC, f"{name} starts with the ESP image magic",
         f"0x{blob[0]:02X} (want 0xE9)")
    chip_id = struct.unpack_from("<H", blob, 12)[0]
    want(chip_id == CHIP_ID_ESP32S3, f"{name} declares ESP32-S3",
         f"chip id {chip_id} (ESP32-S3 is {CHIP_ID_ESP32S3})")
want(parts["partitions.bin"][:2] == PART_MAGIC, "partitions.bin is a partition table",
     parts["partitions.bin"][:2].hex())

print("\n3. The app fits the slot the table gives it")
app0 = None
for line in CSV.read_text().splitlines():
    line = line.split("#")[0].strip()
    if not line:
        continue
    f = [c.strip() for c in line.split(",")]
    if len(f) >= 5 and f[0] == "app0":
        app0 = (int(f[3], 16), int(f[4], 16))
want(app0 is not None, "partitions.csv declares app0", str(app0))
if app0:
    off, size = app0
    want(off == APP_OFF, "app0 begins where the merge puts the app", f"0x{off:X}")
    used = len(parts["firmware.bin"])
    want(used <= size, "firmware.bin fits app0",
         f"{used:,} of {size:,} bytes ({used / size:.1%}), {size - used:,} free")
    want(len(parts["bootloader.bin"]) <= PARTTABLE_OFF, "the bootloader ends before the table",
         f"{len(parts['bootloader.bin']):,} bytes, table at 0x{PARTTABLE_OFF:X}")

print("\n4. The merged file really is the three pieces, byte for byte")
# The repo's release job checks three magic numbers here. Magic numbers prove
# SOMETHING image-shaped is at each offset; these compare the actual bytes, so
# a merge that placed a stale or truncated piece cannot pass.
want(FULL.exists(), "the merged image exists", str(FULL))
full = FULL.read_bytes()
for off, name in ((BOOTLOADER_OFF, "bootloader.bin"), (PARTTABLE_OFF, "partitions.bin"),
                  (APP_OFF, "firmware.bin")):
    blob = parts[name]
    got = full[off:off + len(blob)]
    same = got == blob
    want(same, f"0x{off:06X} is {name}",
         f"{len(blob):,} bytes, sha {hashlib.sha256(blob).hexdigest()[:16]}"
         if same else "the merged image differs from the built piece")
want(len(full) >= APP_OFF + len(parts["firmware.bin"]), "the merged image is not truncated",
     f"{len(full):,} bytes")
# Gaps between the pieces must be erased flash, not another piece's tail.
gap = full[len(parts["bootloader.bin"]):PARTTABLE_OFF]
want(set(gap) <= {0xFF}, "the gap before the partition table is blank",
     f"{len(gap):,} bytes")

print("\n5. The table inside the merged image says what partitions.csv says")
want_rows = []
for line in CSV.read_text().splitlines():
    line = line.split("#")[0].strip()
    if not line:
        continue
    f = [c.strip() for c in line.split(",")]
    if len(f) >= 5:
        want_rows.append((f[0], int(f[3], 16), int(f[4], 16)))
got_rows = []
for i in range(0, 0xC00, 32):
    entry = full[PARTTABLE_OFF + i:PARTTABLE_OFF + i + 32]
    if entry[:2] != PART_MAGIC:
        break
    _, _, _, off, size, label, _ = struct.unpack("<2sBBLL16sL", entry)
    got_rows.append((label.rstrip(b"\x00").decode(), off, size))
want(len(got_rows) == len(want_rows), "the table has every row the CSV declares",
     f"{len(got_rows)} rows, CSV has {len(want_rows)}")
for a, b in zip(want_rows, got_rows):
    want(a == b, f"partition {a[0]}", f"0x{b[1]:X} +0x{b[2]:X}" if a == b else f"csv {a} vs image {b}")
last = max((o + s) for _, o, s in got_rows) if got_rows else 0
want(last <= 0x1000000, "nothing runs past the end of a 16MB chip", f"last byte 0x{last:X}")
overlap = sorted(((o, o + s, n) for n, o, s in got_rows))
want(all(overlap[i][1] <= overlap[i + 1][0] for i in range(len(overlap) - 1)),
     "no two partitions overlap", f"{len(overlap)} regions")

print("\n6. esptool agrees, and says whose firmware this is")
try:
    out = subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3",
                          "image-info", str(BUILD / "firmware.bin")],
                         capture_output=True, text=True, timeout=180)
    text = out.stdout + out.stderr
    want(out.returncode == 0, "esptool parses firmware.bin", f"exit {out.returncode}")
    want("Checksum: " in text and "invalid" not in text.lower(),
         "the app image checksum and hash validate",
         next((l.strip() for l in text.splitlines() if "hecksum" in l), "?"))
    for key in ("Project name:", "App version:"):
        line = next((l.strip() for l in text.splitlines() if key in l), None)
        want(line is not None, f"the image carries its {key.rstrip(':').lower()}", line or "absent")
except Exception as exc:  # noqa: BLE001
    bad("esptool image-info", str(exc))

print(f"\n{'PASS' if not failures else 'FAIL'}: {checks} checks, {len(failures)} failed")
if failures:
    for f in failures:
        print(f"  - {f}")
sys.exit(1 if failures else 0)
