#!/bin/bash
# Full-guide vs medium-guide board on the OpenAI roster, one model per invocation.
#
# Usage: run_guide_full_vs_medium_openai.sh <destination-id> [outdir]
#
# The key is read from ~/aic into the environment here, so it never appears in a
# command line or a log. Reports are written to a .partial file and renamed only
# on a clean exit, so an interrupted suite re-runs instead of being skipped.

set -u

DEST="${1:?destination id required}"
OUTDIR="${2:-/Users/mke/PBuild/Tests/aether_doc_bench/results/guide_full_vs_medium_openai_20260729}"
ROOT=/Users/mke/PBuild
BENCH="$ROOT/Tests/aether_doc_bench"
CONFIG="$BENCH/destinations.guided_2026-07-20.openai.json"
AETHER_BIN="$ROOT/build/bin/aether"

OPENAI_API_KEY="$(tr -d ' \n' < "$HOME/aic")"
export OPENAI_API_KEY

mkdir -p "$OUTDIR"

run_suite() {
  local suite="$1" manifest="$2"
  local json="$OUTDIR/${DEST}_${suite}.json"
  local partial="$json.partial"
  local log="$OUTDIR/${DEST}_${suite}.log"

  if [ -s "$json" ]; then
    echo "[skip] $DEST/$suite already complete"
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
    --output-json "$partial" \
    --text-summary >"$log" 2>&1
  local rc=$?
  if [ "$rc" -eq 0 ] && [ -s "$partial" ]; then
    mv "$partial" "$json"
  fi
  echo "[done] $DEST/$suite rc=$rc"
  return 0
}

run_suite simple  tasks_v2_pos.json
run_suite large   tasks_hard_v2.json
run_suite cs      tasks_cs.json
run_suite nontoon tasks_hard_nontoon.json

echo "[fin ] $DEST"
