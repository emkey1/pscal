#!/usr/bin/env bash
# Apple on-device model (AFM 3 Core) on an iPhone 18 Pro (A20), card-v1, frozen 2026-10-06-1
# binary, B0 suites + traps: the same board as results/afm3_20261008/board_card (M4 iPad Pro).
#   APPLE_FM_BRIDGE_TOKEN=... bash run.sh      (token and device address stay out of tracked files)
set -u
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
for suite in tasks_v2_pos tasks_hard_v2 tasks_hard_nontoon tasks_cs tasks_traps; do
  out="$HERE/afm3__${suite}.json"
  [ -f "$out.done" ] && continue
  echo "$(date -u +%FT%TZ) [run ] $suite" >> "$HERE/status.log"
  python3 tools/aether_doc_bench.py --tasks "Tests/aether_doc_bench/${suite}.json" \
    --destinations-config "$CFG" --destination "$DEST" \
    --doc "card=$CARD" --aether-bin "$BIN" --aether-root "$A" --allow-skew --aether-bin-sha256 "$SHA" \
    --repeats 3 --seed-base 42 --repair-attempts 2 --resume --output-json "$out" --progress \
    > "$HERE/afm3__${suite}.log" 2>&1
  rc=$?
  echo "$(date -u +%FT%TZ) [done] $suite rc=$rc" >> "$HERE/status.log"
  # Done only if it measured its cases: a preflight skip exits 0 with an empty report.
  n=$(python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print(sum(len(v['results']) for d in r['destinations'] for v in d['variants']))" "$out" 2>/dev/null || echo 0)
  if [ $rc -eq 0 ] && [ "$n" -gt 0 ]; then touch "$out.done"; else echo "$(date -u +%FT%TZ) [stop] $suite rc=$rc measured $n case(s); bridge down or locked? not continuing" >> "$HERE/status.log"; exit 1; fi
done
echo "$(date -u +%FT%TZ) COMPLETE" >> "$HERE/status.log"
