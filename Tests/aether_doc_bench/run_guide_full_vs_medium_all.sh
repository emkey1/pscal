#!/bin/bash
# Drive the full-vs-medium guide board across the whole Gemini roster,
# three models in flight at a time (one shared API key, so don't fan out wider).

set -u

OUTDIR="${1:-/Users/mke/PBuild/Tests/aether_doc_bench/results/guide_full_vs_medium_20260729}"
HERE="$(dirname "$0")"
MAX_PARALLEL=3

MODELS=(
  gemini-3.5-flash
  gemini-3.1-flash-lite
  gemini-3-flash-preview
  gemini-2.5-flash
  gemini-2.5-flash-lite
  gemini-3.1-pro-preview
  gemini-2.5-pro
)

mkdir -p "$OUTDIR"

for model in "${MODELS[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$MAX_PARALLEL" ]; do
    wait -n 2>/dev/null || sleep 5
  done
  "$HERE/run_guide_full_vs_medium.sh" "$model" "$OUTDIR" &
done

wait
echo "[ALL DONE] $OUTDIR"
