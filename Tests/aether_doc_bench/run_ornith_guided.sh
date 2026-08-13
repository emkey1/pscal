#!/usr/bin/env bash
# Guided-benchmark run for Ornith-1.0-35B NVFP4 (T'Ra-routed).
# Resumable at per-set granularity; the idempotent T'Ra queue caches completed
# generations so a re-run after a crash reuses finished jobs.
set -u
cd /Users/mke/PBuild || exit 1
BIN=/Users/mke/PBuild/components/aether/build/aether
DEST=Tests/aether_doc_bench/destinations.tra.json
DID=claw1-ornith-1.0-35b-nvfp4
OUT=Tests/aether_doc_bench/results/ornith
mkdir -p "$OUT"

run_set () {
  local name="$1" tasks="$2"
  local out="$OUT/ornith_${name}.json"
  if [ -f "$out" ] && python3 -c "import json,sys;d=json.load(open('$out'));sys.exit(0 if d.get('summary') else 1)" 2>/dev/null; then
    echo "[skip] $name already complete ($out)"; return 0
  fi
  echo "[run ] $name -> $out  ($(date '+%H:%M:%S'))"
  python3 tools/aether_doc_bench.py \
    --tasks "$tasks" \
    --destinations-config "$DEST" --destination "$DID" \
    --docs full,small --repair-attempts 2 \
    --aether-bin "$BIN" \
    --output-json "$out" --progress
  echo "[done] $name rc=$? ($(date '+%H:%M:%S'))"
}

echo "=== Ornith guided bench START $(date) ==="
run_set simple Tests/aether_doc_bench/tasks_v2_pos.json
run_set large  Tests/aether_doc_bench/tasks_hard.json
run_set cs     Tests/aether_doc_bench/tasks_cs.json
echo "=== Ornith guided bench ALL DONE $(date) ==="
