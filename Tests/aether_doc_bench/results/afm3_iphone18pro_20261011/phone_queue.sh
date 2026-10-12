#!/usr/bin/env bash
# Everything still owed on the phone, one board at a time, simple suite only (owner, 2026-10-11):
#   v6 -> g3500 -> g4000 -> Python baseline -> re-run of the card board's unmeasured hard_v2 cases.
# The phone gets unplugged, locked or drops off the network; before each step, and after any step that
# ends early, this waits until the bridge answers /v1/models, however long that takes.
set -u
# Homebrew replaced python3 under a running board on 2026-10-11 (3.14.7 removed, python3 -> 3.15):
# name the interpreter, so a board resumes on the minor version it started on.
export PYTHON=${PYTHON:-/opt/homebrew/bin/python3.14}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
LOG=$HERE/phone_queue.log
export APPLE_FM_BRIDGE_TOKEN=$(/usr/libexec/PlistBuddy -c "Print :bridgeToken" "$HOME/b0-tools/iphone26/org.aether.fmbridge.plist")
note() { echo "$(date -u +%FT%TZ) $*" >> "$LOG"; }
# The bridge's address is private: one line in ~/b0-tools/iphone26/bridge_host (same host as the
# destination's base_url in the untracked destinations file).
BRIDGE_HOST=$(cat "$HOME/b0-tools/iphone26/bridge_host")
bridge_up() {
    curl -s -g -m 15 -H "Authorization: Bearer $APPLE_FM_BRIDGE_TOKEN" "http://$BRIDGE_HOST:8799/v1/models" 2>/dev/null \
        | grep -q '"available"'
}
wait_bridge() {
    local said=0
    until bridge_up; do
        [ $said = 0 ] && { note "[wait] bridge unreachable or model unavailable"; said=1; }
        sleep 30
    done
    [ $said = 1 ] && note "[wait] bridge back"
    return 0
}
step() {  # NAME DONE-FILE COMMAND...
    local name=$1 done_file=$2; shift 2
    [ -f "$done_file" ] && { note "[skip] $name"; return 0; }
    while :; do
        wait_bridge
        note "[run ] $name"
        if "$@"; then note "[done] $name"; return 0; fi
        note "[retry] $name ended early"
        sleep 60
    done
}
G=$HERE/guide_boards
step v6    "$G/v6__tasks_v2_pos.json.done"    bash "$G/run.sh" v6    "$HERE/guides/v6_table_plus.md"
step g3500 "$G/g3500__tasks_v2_pos.json.done" bash "$G/run.sh" g3500 "$HERE/guides/g3500.md"
step g4000 "$G/g4000__tasks_v2_pos.json.done" bash "$G/run.sh" g4000 "$HERE/guides/g4000.md"
step python "$HERE/board_python/python__tasks_v2_pos.json.done" bash "$HERE/board_python/run.sh"
card_hard_rerun() {
    cd "$ROOT" && "$PYTHON" Tests/aether_doc_bench/rerun_nogen_cases.py \
        --report "$HERE/board_card/afm3__tasks_hard_v2.json" --apply >> "$HERE/board_card/afm3__tasks_hard_v2.rerun.log" 2>&1 \
        && touch "$HERE/board_card/afm3__tasks_hard_v2.json.done"
}
step card_hard_v2_rerun "$HERE/board_card/afm3__tasks_hard_v2.json.done" card_hard_rerun
note "QUEUE COMPLETE"
