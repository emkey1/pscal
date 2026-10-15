#!/bin/bash
# Serve + evaluate the six cs_aug19 precision-grid checkpoints against the
# same three suites (simple/large/cs) and docs=none KPI convention used for
# the cs_aug18 precision board, for direct comparability. Calls
# aether_doc_bench.py directly per suite (run_eval_v2.sh only supports the
# simple suite internally, no suite parameter).
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
BENCH_ROOT=$HOME/pscal-bench
QDIR=/storage/aether_eval_queue_cs_aug19_precision
QLOG="$QDIR/queue.log"
mkdir -p "$QDIR"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QLOG"; }

TAGS=(
  qwen3-8b-nothink-4bit-cs-aug19
  qwen3-8b-nothink-8bit-cs-aug19
  qwen3-8b-nothink-16bit-cs-aug19
  qwen35-9b-4bit-cs-aug19
  qwen35-9b-8bit-cs-aug19
  qwen35-9b-16bit-cs-aug19
)

declare -A SUITE_TASKS=(
  [simple]="Tests/aether_doc_bench/tasks_v2_pos.json"
  [large]="Tests/aether_doc_bench/tasks_hard.json"
  [cs]="Tests/aether_doc_bench/tasks_cs.json"
)

log "EVAL QUEUE START — ${#TAGS[@]} models, suites=simple,large,cs docs=none reps=3"

for TAG in "${TAGS[@]}"; do
  DONE_MARK="$QDIR/${TAG}.done"
  if [ -f "$DONE_MARK" ]; then
    log "SKIP $TAG — already evaluated"
    continue
  fi
  MERGED="/storage/$TAG/merged_16bit"
  if [ ! -f "$MERGED/config.json" ]; then
    log "FAIL $TAG — no merged export at $MERGED, skipping"
    continue
  fi

  log "=== serving $TAG ==="
  cd "$W"
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  bash serve_any.sh "$TAG" >> "$QLOG" 2>&1

  READY=0
  for i in $(seq 1 90); do
    curl -s --max-time 5 http://localhost:8019/v1/models 2>/dev/null | grep -q "\"$TAG\"" && { READY=1; break; }
    sleep 10
  done
  if [ "$READY" -ne 1 ]; then
    log "FAIL $TAG — vLLM never became ready, skipping eval"
    docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
    continue
  fi
  log "READY $TAG"

  DEST_CFG="$BENCH_ROOT/Tests/aether_doc_bench/destinations.${TAG}_v2.json"
  cat > "$DEST_CFG" <<JSON
{"destinations":[{"id":"${TAG}-v2","type":"openai_chat_completions","base_url":"http://localhost:8019/v1","api_key":"","model":"${TAG}","temperature":0,"max_output_tokens":2000,"request_timeout_seconds":300,"request_max_retries":3,"retry_backoff_seconds":3,"cooldown_seconds":0}]}
JSON

  cd "$BENCH_ROOT"
  for SUITE in simple large cs; do
    log "--- eval $TAG suite=$SUITE ---"
    python3 -u tools/aether_doc_bench.py \
      --destinations-config "$DEST_CFG" \
      --tasks "${SUITE_TASKS[$SUITE]}" \
      --docs none --repeats 3 --progress \
      --aether-bin $HOME/aether-current/build/aether \
      --output-json "$QDIR/${TAG}_${SUITE}.json" --text-summary >> "$QLOG" 2>&1
  done

  cd "$W"
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  touch "$DONE_MARK"
  log "=== DONE $TAG ==="
done

log "EVAL QUEUE COMPLETE"
