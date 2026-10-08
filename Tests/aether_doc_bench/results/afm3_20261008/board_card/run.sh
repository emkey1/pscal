#!/usr/bin/env bash
# Apple on-device model (AFM 3 Core) on card-v1, frozen 2026-10-06-1 binary, B0 suites + traps.
set -u
S=<scratch>
R=$S/apple-bench/afm3_card_20261008
U=$S/lanes/umb-l10
A=$S/l11-frozen/build/aether
SHA=$(shasum -a 256 "$A" | cut -c1-64)
CARD=$S/l11-frozen/docs/aether_card.md
[ -f "$CARD" ] || CARD=~/git/aether/docs/aether_card.md
export APPLE_FM_BRIDGE_TOKEN="$(cat ~/.config/pscal/apple_fm_bridge.token)"
cd "$U"
for suite in tasks_v2_pos tasks_hard_v2 tasks_hard_nontoon tasks_cs tasks_traps; do
  out="$R/afm3__${suite}.json"
  [ -f "$out.done" ] && continue
  echo "$(date -u +%FT%TZ) [run ] $suite" >> "$R/status.log"
  python3 tools/aether_doc_bench.py --tasks "Tests/aether_doc_bench/${suite}.json" \
    --destinations-config "$S/apple-bench/destinations.apple_fm.local.json" --destination apple-ondevice-afm3 \
    --doc "card=$CARD" --aether-bin "$A" --allow-skew --aether-bin-sha256 "$SHA" \
    --repeats 3 --seed-base 42 --repair-attempts 2 --resume --output-json "$out" --progress \
    > "$R/afm3__${suite}.log" 2>&1
  rc=$?
  echo "$(date -u +%FT%TZ) [done] $suite rc=$rc" >> "$R/status.log"
  # A unit counts as done only if it measured its cases: a preflight skip exits 0
  # with an empty report.
  n=$(python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print(sum(len(v['results']) for d in r['destinations'] for v in d['variants']))" "$out" 2>/dev/null || echo 0)
  if [ $rc -eq 0 ] && [ "$n" -gt 0 ]; then touch "$out.done"; else echo "$(date -u +%FT%TZ) [stop] $suite measured $n case(s); bridge down? not continuing" >> "$R/status.log"; exit 1; fi
done
echo "$(date -u +%FT%TZ) COMPLETE" >> "$R/status.log"
