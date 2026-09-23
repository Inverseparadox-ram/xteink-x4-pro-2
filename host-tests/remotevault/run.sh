#!/bin/sh
# Builds and runs the unlock button's crypto tests. Freestanding: RemoteVault
# pulls in neither mbedTLS nor NimBLE, so the exact bytes that go on the wire
# are checked here against FIPS 180-4 and RFC 4231 with a bare compiler.
#
#   host-tests/remotevault/run.sh
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-remotevault-tests-$(cd ../.. && pwd | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
SRC=../../src/apps_local/remote

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "$SRC/RemoteVault.cpp" test_vault.cpp -o "$BUILD_DIR/test_vault"

"$BUILD_DIR/test_vault"
