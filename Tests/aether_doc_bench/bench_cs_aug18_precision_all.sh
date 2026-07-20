#!/bin/bash
# Run bench_cs_aug18_precision.sh for all 6 cs-aug18-precision-grid models,
# sequentially (one vLLM instance on claw2's single GPU at a time). Run from repo root.
set -u

TAGS=(
  qwen3-8b-nothink-4bit-cs-aug18
  qwen3-8b-nothink-8bit-cs-aug18
  qwen3-8b-nothink-16bit-cs-aug18
  qwen35-9b-4bit-cs-aug18
  qwen35-9b-8bit-cs-aug18
  qwen35-9b-16bit-cs-aug18
)

LOG=Tests/aether_doc_bench/out/cs_aug18_precision/bench_all.log
mkdir -p "$(dirname "$LOG")"

log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }

OUTDIR=Tests/aether_doc_bench/out/cs_aug18_precision

log "BENCH ALL START — ${#TAGS[@]} models"
for tag in "${TAGS[@]}"; do
  if [ -f "$OUTDIR/${tag}_simple.json" ] && [ -f "$OUTDIR/${tag}_large.json" ] && [ -f "$OUTDIR/${tag}_cs.json" ]; then
    log "SKIP $tag — all 3 suite results already present"
    continue
  fi
  log "=== benchmarking $tag ==="
  if ./Tests/aether_doc_bench/bench_cs_aug18_precision.sh "$tag" >> "$LOG" 2>&1; then
    log "OK $tag"
  else
    log "FAIL $tag (rc=$?) — see $LOG for details, continuing to next model"
  fi
done
log "BENCH ALL COMPLETE"
