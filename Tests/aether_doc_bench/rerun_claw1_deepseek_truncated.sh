#!/bin/bash
# Re-run only the cases the 2026-08-07 DeepSeek-V4-Flash sweep lost to max_tokens
# truncation (finish_reason=length, content=null), now that max_output_tokens is
# 131072 instead of 32768.
#
# Raising the cap cannot change a case that already completed -- every passing run
# finished far under 32768 tokens -- so merging these results back yields a board
# equivalent to having run everything at the higher budget.
#
# Run from the repo root (PBuild/).
set -uo pipefail

CFG=Tests/aether_doc_bench/destinations.claw1_deepseek_v4_flash.json
DEST=claw1-deepseek-v4-flash-0731
OUTDIR=Tests/aether_doc_bench/out/claw1_deepseek_v4_flash/rerun
AETHER_BIN=${AETHER_BIN:-/usr/local/bin/aether}
mkdir -p "$OUTDIR"

run() {
    local suite=$1 doc=$2; shift 2
    local args=()
    for t in "$@"; do args+=(--task "$t"); done
    echo "=== $suite / $doc : $* ==="
    python3 tools/aether_doc_bench.py \
        --destinations-config "$CFG" \
        --destination "$DEST" \
        --tasks "Tests/aether_doc_bench/$suite.json" \
        --docs "$doc" \
        --repair-attempts 2 \
        --aether-bin "$AETHER_BIN" \
        --output-json "$OUTDIR/${suite}.${doc}.json" \
        "${args[@]}" \
        --text-summary --progress
    echo "=== $suite / $doc done ==="
}

run tasks_v2_pos       medium two_type_methods
run tasks_hard_v2      full   hard_account_ledger hard_payroll_nested hard_sensor_streak hard_word_lengths
run tasks_hard_v2      medium hard_account_ledger hard2_route_legs
run tasks_cs           full   cs_merge_sort
run tasks_cs           medium cs_quick_sort
run tasks_hard_nontoon full   hard2_inventory_rollup hard2_rle_roundtrip hard2_heap_sort
run tasks_hard_nontoon medium hard2_inventory_rollup hard2_rle_roundtrip

echo "RERUN COMPLETE"
