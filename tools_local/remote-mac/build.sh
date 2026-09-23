#!/bin/sh
# Builds crossplay-unlock. Run this ON THE MAC -- it needs Swift, CoreBluetooth
# and the macOS SDK, none of which exist on the reader's build host.
#
#   ./build.sh && sudo cp crossplay-unlock /usr/local/bin/
set -e
cd "$(dirname "$0")"

# The Info.plist is linked INTO the binary rather than sitting beside it. A
# command-line tool with no bundle has nowhere else to put
# NSBluetoothAlwaysUsageDescription, and without that string macOS denies
# Bluetooth rather than asking -- silently, which is the worst of both.
swiftc -O CrossPlayUnlock.swift -o crossplay-unlock \
  -framework CoreBluetooth -framework CryptoKit -framework CoreGraphics \
  -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist

# Ad-hoc signing, so the Bluetooth permission sticks to THIS binary. Unsigned,
# macOS attributes the permission to whatever launched it -- which is Terminal
# when you test it and launchd when it matters, and those are two different
# grants.
codesign -s - --force --options runtime crossplay-unlock

echo "built ./crossplay-unlock"
