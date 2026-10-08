#!/usr/bin/env bash
# Guide-variant experiment for the 4K on-device tier: AFM 3 Core, frozen 2026-10-06-1 binary.
set -u
S=<scratch>
E=$S/apple-bench/guide_exp; G=$S/apple-bench/guides; U=$S/lanes/umb-l10
A=$S/l11-frozen/build/aether; SHA=$(shasum -a 256 "$A" | cut -c1-64)
export APPLE_FM_BRIDGE_TOKEN="$(cat ~/.config/pscal/apple_fm_bridge.token)"
declare -a V2POS=(classify_scores range_loop_squares nested_multiply_grid inline_if_average percentage_format type_init_point dynamic_array_rollup array_minmax toon_parse_file_simple recursion_factorial)
declare -a HARD=(hard_expense_outliers hard_word_lengths hard_append_growth)
declare -a CS=(cs_factorial cs_gcd cs_fizzbuzz)
cd "$U"
for v in ${VARIANTS:-v0_card v1_card_table v5_card_toon v2_card_table_toon v3_template v4_minimal}; do
  for suite in tasks_v2_pos tasks_hard_v2 tasks_cs; do
    case $suite in tasks_v2_pos) T=("${V2POS[@]}");; tasks_hard_v2) T=("${HARD[@]}");; tasks_cs) T=("${CS[@]}");; esac
    out="$E/${v}__${suite}.json"; [ -f "$out.done" ] && continue
    args=(); for t in "${T[@]}"; do args+=(--task "$t"); done
    echo "$(date -u +%FT%TZ) [run ] $v $suite" >> "$E/status.log"
    python3 tools/aether_doc_bench.py --tasks "Tests/aether_doc_bench/${suite}.json" "${args[@]}" \
      --destinations-config "$S/apple-bench/destinations.apple_fm.local.json" --destination apple-ondevice-afm3 \
      --doc "${v}=$G/${v}.md" --aether-bin "$A" --allow-skew --aether-bin-sha256 "$SHA" \
      --repeats 2 --seed-base 42 --repair-attempts 2 --resume --output-json "$out" > "$E/${v}__${suite}.log" 2>&1
    rc=$?
    n=$(python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print(sum(len(v['results']) for d in r['destinations'] for v in d['variants']))" "$out" 2>/dev/null || echo 0)
    if [ "$rc" -eq 0 ] && [ "$n" -gt 0 ]; then touch "$out.done"; echo "$(date -u +%FT%TZ) [done] $v $suite n=$n" >> "$E/status.log"
    else echo "$(date -u +%FT%TZ) [stop] $v $suite rc=$rc n=$n" >> "$E/status.log"; exit 1; fi
  done
done
echo "$(date -u +%FT%TZ) COMPLETE" >> "$E/status.log"
