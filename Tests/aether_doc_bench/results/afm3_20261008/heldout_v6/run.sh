#!/usr/bin/env bash
# Held-out confirmation: V6 (card + translation table + extra rows) on the 57 tasks the
# guide experiment never saw. Matched to the AFM3 card board: frozen binary, seeds 42-44.
set -u
S=<scratch>
H=$S/apple-bench/heldout_v6; G=$S/apple-bench/guides; U=$S/lanes/umb-l10
A=$S/l11-frozen/build/aether; SHA=$(shasum -a 256 "$A" | cut -c1-64)
until grep -q COMPLETE "$S/apple-bench/guide_exp/status.log" 2>/dev/null && [ "$(grep -c COMPLETE "$S/apple-bench/guide_exp/status.log")" -ge 2 ]; do sleep 20; done
export APPLE_FM_BRIDGE_TOKEN="$(cat ~/.config/pscal/apple_fm_bridge.token)"
cd "$U"
DEV="classify_scores range_loop_squares nested_multiply_grid inline_if_average percentage_format type_init_point dynamic_array_rollup array_minmax toon_parse_file_simple recursion_factorial hard_expense_outliers hard_word_lengths hard_append_growth cs_factorial cs_gcd cs_fizzbuzz"
for suite in tasks_v2_pos tasks_hard_v2 tasks_hard_nontoon tasks_cs; do
  out="$H/v6__${suite}.json"; [ -f "$out.done" ] && continue
  args=()
  for t in $(python3 -c "import json,sys; print(' '.join(t['id'] for t in json.load(open(sys.argv[1]))['tasks']))" "Tests/aether_doc_bench/${suite}.json"); do
    case " $DEV " in *" $t "*) ;; *) args+=(--task "$t");; esac
  done
  echo "$(date -u +%FT%TZ) [run ] $suite ${#args[@]} args" >> "$H/status.log"
  python3 tools/aether_doc_bench.py --tasks "Tests/aether_doc_bench/${suite}.json" "${args[@]}" \
    --destinations-config "$S/apple-bench/destinations.apple_fm.local.json" --destination apple-ondevice-afm3 \
    --doc "v6_table_plus=$G/v6_table_plus.md" --aether-bin "$A" --allow-skew --aether-bin-sha256 "$SHA" \
    --repeats 3 --seed-base 42 --repair-attempts 2 --resume --output-json "$out" > "$H/v6__${suite}.log" 2>&1
  rc=$?
  n=$(python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print(sum(len(v['results']) for d in r['destinations'] for v in d['variants']))" "$out" 2>/dev/null || echo 0)
  if [ "$rc" -eq 0 ] && [ "$n" -gt 0 ]; then touch "$out.done"; echo "$(date -u +%FT%TZ) [done] $suite n=$n" >> "$H/status.log"
  else echo "$(date -u +%FT%TZ) [stop] $suite rc=$rc n=$n" >> "$H/status.log"; exit 1; fi
done
echo "$(date -u +%FT%TZ) COMPLETE" >> "$H/status.log"
