#!/bin/sh
# Builds and runs the clock app's freestanding tests. No device and no RTC:
# ClockCore is pure arithmetic over numbers the Activity hands in, which is the
# only reason a calendar can be tested at all.
#
#   host-tests/clock/run.sh
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-clock-tests-$(cd ../.. && pwd | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
SRC=../../src/apps_local/clock

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$SRC/ClockCore.cpp" test_core.cpp -o "$BUILD_DIR/test_core"

"$BUILD_DIR/test_core"
