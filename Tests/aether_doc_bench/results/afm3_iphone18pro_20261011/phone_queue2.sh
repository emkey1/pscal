#!/usr/bin/env bash
# Follow-on to phone_queue.sh: a third draft (g3700), written from the g3500/g4000 results,
# on the simple suite. Starts when phone_queue.sh has finished; same wait-for-bridge rule.
set -u
export PYTHON=${PYTHON:-/opt/homebrew/bin/python3.14}
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=$HERE/phone_queue.log
while screen -ls | grep -q "a20queue"; do sleep 30; done
export APPLE_FM_BRIDGE_TOKEN=$(/usr/libexec/PlistBuddy -c "Print :bridgeToken" "$HOME/b0-tools/iphone26/org.aether.fmbridge.plist")
BRIDGE_HOST=$(cat "$HOME/b0-tools/iphone26/bridge_host")
note() { echo "$(date -u +%FT%TZ) $*" >> "$LOG"; }
bridge_up() { curl -s -g -m 15 -H "Authorization: Bearer $APPLE_FM_BRIDGE_TOKEN" "http://$BRIDGE_HOST:8799/v1/models" 2>/dev/null | grep -q '"available"'; }
for name in "$@"; do
    [ -f "$HERE/guide_boards/${name}__tasks_v2_pos.json.done" ] && continue
    while :; do
        until bridge_up; do sleep 30; done
        note "[run ] $name"
        if bash "$HERE/guide_boards/run.sh" "$name" "$HERE/guides/$name.md"; then note "[done] $name"; break; fi
        note "[retry] $name ended early"; sleep 60
    done
done
note "QUEUE2 COMPLETE"
