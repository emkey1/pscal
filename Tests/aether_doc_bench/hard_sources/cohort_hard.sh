#!/bin/bash
set -u
say(){ echo "$(date +%H:%M:%S) $*"; }
MST=/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724
RUN=/home/claw/pscal-bench/run_eval_hard.sh
serve(){ local TAG=$1; local MODEL=$2; shift 2
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
    -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
    --model "$MODEL" --served-model-name "$TAG" --dtype bfloat16 --max-model-len 16384 \
    --gpu-memory-utilization 0.85 --enforce-eager --port 8000 "$@" >/dev/null
}
teardown(){ docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1; }
say "=== COHORT-HARD QUEUED: waiting for 3x thinking chain (PHASE2-3X DONE) ==="
WAITED=0
while ! grep -q "PHASE2-3X DONE" /tmp/phase2_3x.log 2>/dev/null; do
  sleep 60; WAITED=$((WAITED+1))
  if [ $WAITED -gt 240 ] && ! docker ps --format '{{.Names}}' | grep -qE 'aether-bench-vllm-q7|aether-train'; then
    say "WARN: PHASE2-3X DONE not seen after ${WAITED}m but GPU free -> proceeding"; break
  fi
done
while docker ps --format '{{.Names}}' | grep -qE 'aether-bench-vllm-q7|aether-train'; do sleep 30; done
say "=== GPU free -> hard cohort eval ==="
for TAG in qwen7b-ml1x qwen14b-ml2x q3ca3b-ml1x; do
  [ -d /storage/$TAG/merged_16bit ] || { say "SKIP $TAG (missing)"; continue; }
  say "### SERVE+EVAL $TAG (hard, plain) ###"
  serve "$TAG" /storage/$TAG/merged_16bit
  bash "$RUN" "$TAG" 4000 0 plain 2>&1
  teardown
done
if [ -d /storage/mistral24b-ml2x/merged_16bit ]; then
  say "### SERVE+EVAL mistral24b-ml2x (hard, plain, tekken) ###"
  serve mistral24b-ml2x /storage/mistral24b-ml2x/merged_16bit --tokenizer "$MST" --tokenizer-mode mistral
  bash "$RUN" mistral24b-ml2x 4000 0 plain 2>&1
  teardown
fi
if [ -d /storage/qwen3-8b-think/merged_16bit ]; then
  say "### SERVE qwen3-8b-think (hard: think vs nothink @0.6) ###"
  serve qwen3-8b-think /storage/qwen3-8b-think/merged_16bit
  bash "$RUN" qwen3-8b-think 4000 0.6 think 2>&1
  bash "$RUN" qwen3-8b-think 4000 0.6 nothink 2>&1
  teardown
fi
say "### COHORT-HARD DONE ###"
