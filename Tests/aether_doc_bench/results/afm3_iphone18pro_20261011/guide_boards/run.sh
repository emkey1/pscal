#!/usr/bin/env bash
# One guide variant on the simple suite (tasks_v2_pos, 35 tasks x 3 repeats) for the phone's
# on-device model. Owner, 2026-10-11: only the simple items until they are near perfect.
#   APPLE_FM_BRIDGE_TOKEN=... bash run.sh <variant-name> <guide.md>
set -u
# Homebrew replaced python3 under a running board on 2026-10-11 (3.14.7 removed, python3 -> 3.15):
# name the interpreter, so a board resumes on the minor version it started on.
export PYTHON=${PYTHON:-/opt/homebrew/bin/python3.14}
NAME=${1:?variant name}; GUIDE=${2:?guide file}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
A=${AETHER_ROOT:-$HOME/aether-6659679}
BIN=$A/build/aether
SHA=$(shasum -a 256 "$BIN" | cut -c1-64)
CFG=${CFG:-Tests/aether_doc_bench/destinations.apple_fm_iphone.local.json}
DEST=${DEST:-apple-ondevice-afm3adv-iphone18pro}
: "${APPLE_FM_BRIDGE_TOKEN:?set APPLE_FM_BRIDGE_TOKEN}"
case "$GUIDE" in /*) ;; *) GUIDE="$PWD/$GUIDE" ;; esac
cd "$ROOT" || exit 1
out="$HERE/${NAME}__tasks_v2_pos.json"
[ -f "$out.done" ] && { echo "$NAME already done"; exit 0; }
echo "$(date -u +%FT%TZ) [run ] $NAME" >> "$HERE/status.log"
"$PYTHON" tools/aether_doc_bench.py --tasks Tests/aether_doc_bench/tasks_v2_pos.json \
  --destinations-config "$CFG" --destination "$DEST" \
  --doc "$NAME=$GUIDE" --aether-bin "$BIN" --aether-root "$A" --allow-skew --aether-bin-sha256 "$SHA" \
  --repeats 3 --seed-base 42 --repair-attempts 2 --resume --output-json "$out" --progress \
  > "$HERE/${NAME}__tasks_v2_pos.log" 2>&1
rc=$?
n=$("$PYTHON" -c "import json,sys; r=json.load(open(sys.argv[1])); print(sum(len(v['results']) for d in r['destinations'] for v in d['variants']))" "$out" 2>/dev/null || echo 0)
echo "$(date -u +%FT%TZ) [done] $NAME rc=$rc cases=$n" >> "$HERE/status.log"
if [ $rc -eq 0 ] && [ "$n" -gt 0 ]; then touch "$out.done"; exit 0; fi
# A report with no cases is a failed model check (bridge down), not a result: set it aside so
# the next attempt starts clean. A partial report (rc 3) stays, and --resume continues it.
if [ "$n" -eq 0 ] && [ -f "$out" ]; then mkdir -p "$HERE/rejected"; mv "$out" "$HERE/rejected/${NAME}__zero_cases_$(date -u +%H%M%S).json"; fi
exit 1
