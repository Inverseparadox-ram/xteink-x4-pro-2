#!/bin/sh
# Builds and runs the Weather app's freestanding tests. No device and no
# PlatformIO needed: WeatherCore is freestanding C++17 and deliberately knows
# nothing about ArduinoJson, HalStorage or the network -- if it ever reaches
# for any of them, this build fails loudly rather than the suite going quiet.
#
#   host-tests/weather/run.sh
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-weather-tests-$(cd ../.. && pwd | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
SRC=../../src/apps_local/weather

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$SRC/WeatherCore.cpp" test_core.cpp -o "$BUILD_DIR/test_core"

"$BUILD_DIR/test_core"
