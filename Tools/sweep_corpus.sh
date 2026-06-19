#!/bin/bash
# Corpus-size x model-size sweep: {7B,14B} x {1x,2x corpus}, no-guide ("none") KPI.
# Recipe constant per model; only the corpus varies (1x=183, 2x=383 records, v2-clean).
# Launch via djob: stdout -> /tmp/sweep_corpus.log, rc -> /tmp/sweep_corpus.done.
set -u
W=/home/claw/training/aether-qwen-coder-30b-unsloth
cd "$W" || exit 2
# Resolve local snapshot dirs (Unsloth can't resolve the bare HF id offline).
M7B=$(ls -d /storage/hf/hub/models--Qwen--Qwen2.5-Coder-7B-Instruct/snapshots/*/ 2>/dev/null | head -1)
M14B=$(ls -d /storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/*/ 2>/dev/null | head -1)
say() { echo "$(date '+%H:%M:%S') $*"; }
say "=== SWEEP START ==="
say "7B model:  $M7B"
say "14B model: $M14B"
[ -d "$M7B" ] && [ -d "$M14B" ] || { say "!! missing snapshot dir; abort"; exit 3; }

run_one() {
  local TAG=$1 MODEL=$2 R=$3 ALPHA=$4 DATA=$5 EP=$6
  say ""
  say "### TRAIN $TAG  r=$R a=$ALPHA data=$DATA ep=$EP ###"
  if [ ! -f "$W/$DATA/aether_instruction_sft.jsonl" ]; then
    say "!! MISSING dataset $W/$DATA — skipping $TAG"; return 1
  fi
  docker rm -f "aether-train-$TAG" >/dev/null 2>&1
  bash train_any.sh "$TAG" "$MODEL" "$R" "$ALPHA" "$DATA" "$EP" 2>&1
  say "### waiting on aether-train-$TAG ###"
  docker wait "aether-train-$TAG" 2>&1
  docker logs --tail 12 "aether-train-$TAG" 2>&1
  if [ ! -d "/storage/$TAG/merged_16bit" ]; then
    say "!! TRAIN FAILED $TAG — no merged_16bit, skipping eval"; return 1
  fi
  say "### TRAIN DONE $TAG ###"
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  bash serve_any.sh "$TAG" 2>&1
  say "### EVAL $TAG docs=none rep=3 ###"
  bash run_eval_v2.sh "$TAG" none 3 2>&1
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  say "### RUN COMPLETE $TAG ###"
}

run_one qwen7b-ml1x  "$M7B"  32 64  data_qwen_ml1x 3
run_one qwen7b-ml2x  "$M7B"  32 64  data_qwen_ml2x 3
run_one qwen14b-ml1x "$M14B" 64 128 data_qwen_ml1x 4
run_one qwen14b-ml2x "$M14B" 64 128 data_qwen_ml2x 4

say ""
say "=== SWEEP DONE — eval JSONs at /tmp/<tag>_v2_none.json; text-summaries above ==="
for T in qwen7b-ml1x qwen7b-ml2x qwen14b-ml1x qwen14b-ml2x; do
  J=/tmp/${T}_v2_none.json
  [ -f "$J" ] && say "  $T eval json present: $J" || say "  $T : (no eval json)"
done
say "=== SWEEP END ==="
