#!/usr/bin/env bash
# Python baseline: the same on-device model solves the same tasks in Python 3 (no guide), sandboxed.
# Separates "the model cannot solve the task" from "the model cannot write Aether from the card".
#   APPLE_FM_BRIDGE_TOKEN=... bash run.sh      (token and device address stay out of tracked files)
set -u
# Homebrew replaced python3 under a running board on 2026-10-11 (3.14.7 removed, python3 -> 3.15):
# name the interpreter, so a board resumes on the minor version it started on.
export PYTHON=${PYTHON:-/opt/homebrew/bin/python3.14}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
A=${AETHER_ROOT:-$HOME/aether-6659679}
BIN=$A/build/aether
SHA=$(shasum -a 256 "$BIN" | cut -c1-64)
CARD=$A/docs/aether_card.md
CFG=${CFG:-Tests/aether_doc_bench/destinations.apple_fm_iphone.local.json}
DEST=${DEST:-apple-ondevice-afm3adv-iphone18pro}
: "${APPLE_FM_BRIDGE_TOKEN:?set APPLE_FM_BRIDGE_TOKEN}"
cd "$ROOT" || exit 1
for suite in tasks_v2_pos; do  # simple items only (owner, 2026-10-11)
  out="$HERE/python__${suite}.json"
  [ -f "$out.done" ] && continue
  echo "$(date -u +%FT%TZ) [run ] $suite" >> "$HERE/status.log"
  "$PYTHON" tools/aether_doc_bench.py --tasks "Tests/aether_doc_bench/${suite}.json" \
    --destinations-config "$CFG" --destination "$DEST" \
    --docs none --skip-aether --python-baseline --aether-bin "$BIN" --aether-root "$A" --allow-skew --aether-bin-sha256 "$SHA" \
    --repeats 3 --seed-base 42 --repair-attempts 2 --resume --output-json "$out" --progress \
    > "$HERE/python__${suite}.log" 2>&1
  rc=$?
  echo "$(date -u +%FT%TZ) [done] $suite rc=$rc" >> "$HERE/status.log"
  # Done only if it measured its cases: a preflight skip exits 0 with an empty report.
  n=$("$PYTHON" -c "import json,sys; r=json.load(open(sys.argv[1])); print(sum(len(v.get('python_baseline_results') or []) for d in r['destinations'] for v in d['variants']))" "$out" 2>/dev/null || echo 0)
  if [ $rc -eq 0 ] && [ "$n" -gt 0 ]; then touch "$out.done"; else echo "$(date -u +%FT%TZ) [stop] $suite rc=$rc measured $n case(s); bridge down or locked? not continuing" >> "$HERE/status.log"; exit 1; fi
done
echo "$(date -u +%FT%TZ) COMPLETE" >> "$HERE/status.log"
