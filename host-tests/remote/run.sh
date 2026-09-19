#!/bin/sh
# Builds and runs the BLE remote's freestanding tests. No device, no radio and
# no PlatformIO: RemoteCore knows nothing about NimBLE, and if it ever reaches
# for it this build fails loudly rather than the suite going quiet.
#
#   host-tests/remote/run.sh
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-remote-tests-$(cd ../.. && pwd | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
SRC=../../src/apps_local/remote

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$SRC/RemoteCore.cpp" test_core.cpp -o "$BUILD_DIR/test_core"

"$BUILD_DIR/test_core"
