#!/bin/bash
# Drive the full-vs-medium guide board across the OpenAI roster.
#
# Three models in flight. Each model keeps its own prompt cache warm by running
# its suites back-to-back, which is where ~97% of the input tokens get served
# from, so widening this past 3 buys little and risks rate limits.

set -u

OUTDIR="${1:-/Users/mke/PBuild/Tests/aether_doc_bench/results/guide_full_vs_medium_openai_20260729}"
HERE="$(dirname "$0")"
MAX_PARALLEL=3

MODELS=(
  gpt-5.6-luna
  gpt-5.6-sol
  gpt-5.6-terra
  gpt-5.5
  gpt-5.4
  gpt-5.4-mini
  gpt-5.2
  gpt-5.1
  o4-mini
  o3
)

mkdir -p "$OUTDIR"

for model in "${MODELS[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$MAX_PARALLEL" ]; do
    wait -n 2>/dev/null || sleep 5
  done
  "$HERE/run_guide_full_vs_medium_openai.sh" "$model" "$OUTDIR" &
done

wait
echo "[ALL DONE] $OUTDIR"
