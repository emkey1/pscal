#!/bin/bash
# Full-guide vs medium-guide board, one model per invocation.
#
# Usage: run_guide_full_vs_medium.sh <destination-id> [outdir]
#
# Runs all four task suites (simple/large/cs/nontoon) against --docs full,medium
# and writes one JSON report per suite. Designed to be launched once per model so
# several models can run concurrently without sharing a single long process.

set -u

DEST="${1:?destination id required}"
OUTDIR="${2:-/Users/mke/PBuild/Tests/aether_doc_bench/results/guide_full_vs_medium_20260729}"
ROOT=/Users/mke/PBuild
BENCH="$ROOT/Tests/aether_doc_bench"
CONFIG="$BENCH/destinations.guided_2026-07-20.gemini.json"
AETHER_BIN="$ROOT/build/bin/aether"

mkdir -p "$OUTDIR"

run_suite() {
  local suite="$1" manifest="$2"
  local json="$OUTDIR/${DEST}_${suite}.json"
  local log="$OUTDIR/${DEST}_${suite}.log"

  if [ -s "$json" ]; then
    echo "[skip] $DEST/$suite already has $json"
    return 0
  fi

  echo "[run ] $DEST/$suite -> $json"
  python3 "$ROOT/tools/aether_doc_bench.py" \
    --tasks "$BENCH/$manifest" \
    --destinations-config "$CONFIG" \
    --destination "$DEST" \
    --docs full,medium \
    --repair-attempts 2 \
    --aether-bin "$AETHER_BIN" \
    --output-json "$json" \
    --text-summary >"$log" 2>&1
  local rc=$?
  echo "[done] $DEST/$suite rc=$rc"
  return 0
}

run_suite simple  tasks_v2_pos.json
run_suite large   tasks_hard_v2.json
run_suite cs      tasks_cs.json
run_suite nontoon tasks_hard_nontoon.json

echo "[fin ] $DEST"
