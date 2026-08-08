#!/bin/bash
# Benchmark DeepSeek-V4-Flash-0731 (vLLM across claw1 rank0/API + claw2 rank1) on
# the current guide-size board's methodology: four suites, --docs medium,
# --repair-attempts 2. Run from the repo root (PBuild/).
set -uo pipefail

CFG=Tests/aether_doc_bench/destinations.glm47flash_tra.json
DEST=glm47flash-tra-cardspec
OUTDIR=Tests/aether_doc_bench/out/glm47flash_cardspec
AETHER_BIN=${AETHER_BIN:-/usr/local/bin/aether}
mkdir -p "$OUTDIR"

for suite in tasks_v2_pos tasks_hard_v2 tasks_cs tasks_hard_nontoon; do
    echo "=== $suite ==="
    python3 tools/aether_doc_bench.py \
        --destinations-config "$CFG" \
        --destination "$DEST" \
        --tasks "Tests/aether_doc_bench/$suite.json" \
        --docs medium \
        --repair-attempts 2 \
        --aether-bin "$AETHER_BIN" \
        --output-json "$OUTDIR/$suite.json" \
        --text-summary --progress
    echo "=== $suite done (exit $?) ==="
done
echo "ALL SUITES COMPLETE"
