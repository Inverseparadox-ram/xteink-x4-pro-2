#!/bin/sh
# Brings the Mac half up to date in one step: pull, build, install, restart,
# then show what the helper reports. Run it from anywhere:
#
#   ~/xteink-x4-pro-2/tools_local/remote-mac/update.sh
#
# It asks for your Mac password once, for the copy into /usr/local/bin.
set -e
cd "$(dirname "$0")"

echo "== pulling"
git pull --ff-only

echo "== building"
./build.sh

echo "== installing"
sudo mkdir -p /usr/local/bin
sudo cp crossplay-unlock /usr/local/bin/

echo "== restarting the helper"
AGENT="$HOME/Library/LaunchAgents/com.crossplay.unlock.plist"
[ -f "$AGENT" ] || cp com.crossplay.unlock.plist "$AGENT"
# kickstart restarts a loaded agent; bootstrap loads one that is not.
launchctl kickstart -k "gui/$(id -u)/com.crossplay.unlock" 2>/dev/null ||
  launchctl bootstrap "gui/$(id -u)" "$AGENT"
sleep 3

echo "== status"
crossplay-unlock status
echo
echo "== last lines of /tmp/crossplay-unlock.log"
tail -8 /tmp/crossplay-unlock.log
