#!/bin/sh
# Builds and runs the Stocks app's freestanding tests. StocksCore knows nothing
# about ArduinoJson, HalStorage or the network, and this build is what keeps it
# that way: reaching for any of them fails here rather than quietly.
#
#   host-tests/stocks/run.sh
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-stocks-tests-$(cd ../.. && pwd | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
SRC=../../src/apps_local/stocks

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$SRC/StocksCore.cpp" test_core.cpp -o "$BUILD_DIR/test_core"

"$BUILD_DIR/test_core"
