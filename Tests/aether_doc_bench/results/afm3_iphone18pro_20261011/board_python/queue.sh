#!/usr/bin/env bash
# Python baseline on the simple suite, after the card board and the guide boards.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
while screen -ls | grep -qE "a20card|a20guides"; do sleep 30; done
export APPLE_FM_BRIDGE_TOKEN=$(/usr/libexec/PlistBuddy -c "Print :bridgeToken" "$HOME/b0-tools/iphone26/org.aether.fmbridge.plist")
exec bash "$HERE/run.sh"
