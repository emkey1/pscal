#!/usr/bin/env bash
# Full guided-benchmark suite (T'Ra-routed), resumable at per-(model,set) granularity.
#
# Runs every destination in the config across all three instruments
# (simple / large / cs), both guide sizes, repair on -- the canonical guided
# board (Compiled/Correct/Retried/Fixed), see components/aether/docs/aether_guided_benchmark.md.
#
# PREREQ before running (this rerun is meant to follow today's doc+compiler fixes):
#   1. Land the compiler/guide fixes.
#   2. Rebuild the aether binary: the one at $BIN below.
#   3. Confirm each model is served + routable (see run_full_guided_readiness.sh).
# The T'Ra queue is idempotent, so a crash/re-run reuses completed generations;
# per-(model,set) output files are skipped once they carry a summary.
#
# Usage: run_full_guided.sh [DESTINATIONS_JSON] [DEST_ID ...]
#   default config: destinations.tra.json (claw1+claw2 cohort incl Ornith-1.0)
#   optional DEST_IDs restrict to a subset; omit to run the whole cohort.
set -u
cd /Users/mke/PBuild || exit 1

BIN=/Users/mke/PBuild/components/aether/build/aether
DEST_CONFIG="${1:-Tests/aether_doc_bench/destinations.tra.json}"
shift 2>/dev/null || true
OUT=Tests/aether_doc_bench/results/full
mkdir -p "$OUT"

if [ ! -x "$BIN" ]; then
  echo "!! aether binary missing/not built: $BIN"
  echo "!! Rebuild it (post-fixes) before running the suite. Aborting."
  exit 1
fi
echo "aether: $($BIN --version 2>&1 | head -1)"

# Destination ids: explicit args, else every id in the config.
if [ "$#" -gt 0 ]; then
  DEST_IDS="$*"
else
  DEST_IDS="$(python3 -c "import json,sys;print(' '.join(d['id'] for d in json.load(open('$DEST_CONFIG'))['destinations']))")"
fi

# Task instruments (the three guided-benchmark boards).
SETS="simple:Tests/aether_doc_bench/tasks_v2_pos.json large:Tests/aether_doc_bench/tasks_hard.json cs:Tests/aether_doc_bench/tasks_cs.json"

complete () {  # $1 = output json -> 0 if it already has a summary
  [ -f "$1" ] && python3 -c "import json,sys;d=json.load(open('$1'));sys.exit(0 if d.get('summary') else 1)" 2>/dev/null
}

echo "=== FULL guided bench START $(date) ==="
echo "config: $DEST_CONFIG"
echo "models: $DEST_IDS"
for did in $DEST_IDS; do
  for pair in $SETS; do
    name="${pair%%:*}"; tasks="${pair#*:}"
    out="$OUT/${did}_${name}.json"
    if complete "$out"; then echo "[skip] $did / $name"; continue; fi
    echo "[run ] $did / $name -> $out ($(date '+%H:%M:%S'))"
    # Best-effort: a down/unroutable model must not stop the suite.
    python3 tools/aether_doc_bench.py \
      --tasks "$tasks" \
      --destinations-config "$DEST_CONFIG" --destination "$did" \
      --docs full,small --repair-attempts 2 \
      --aether-bin "$BIN" \
      --output-json "$out" --progress \
      || echo "[warn] $did / $name failed (rc=$?) -- continuing"
  done
done
echo "=== FULL guided bench ALL DONE $(date) ==="
echo "per-(model,set) results in $OUT/ ; regenerate the boards in aether_guided_benchmark.md from these."
