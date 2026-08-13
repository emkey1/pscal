#!/bin/bash
# A3B (Qwen3-Coder-30B-A3B MoE, 3B active) corpus-size sweep: 1x vs 2x, no-guide KPI.
# Same prior recipe as q3ca3b-v8e (r32/a64/3ep, no INSTR/RESP), only the corpus varies.
# Tests whether a sparse MoE absorbs the compositional corpus like the dense 14B did.
# Launch via djob: stdout -> /tmp/sweep_a3b.log, rc -> /tmp/sweep_a3b.done.
set -u
W=/home/claw/training/aether-qwen-coder-30b-unsloth
cd "$W" || exit 2
MA3B=/storage/archive/qwen3-coder-30b-a3b-model-mirror
say() { echo "$(date '+%H:%M:%S') $*"; }
say "=== A3B SWEEP START ==="
say "model: $MA3B"
[ -d "$MA3B" ] && [ -f "$MA3B/config.json" ] || { say "!! missing A3B model dir/config; abort"; exit 3; }

run_one() {
  local TAG=$1 DATA=$2
  say ""
  say "### TRAIN $TAG  r=32 a=64 data=$DATA ep=3 ###"
  [ -f "$W/$DATA/aether_instruction_sft.jsonl" ] || { say "!! missing dataset $DATA"; return 1; }
  docker rm -f "aether-train-$TAG" >/dev/null 2>&1
  bash train_any.sh "$TAG" "$MA3B" 32 64 "$DATA" 3 2>&1
  say "### waiting on aether-train-$TAG ###"
  docker wait "aether-train-$TAG" 2>&1
  docker logs --tail 12 "aether-train-$TAG" 2>&1
  [ -d "/storage/$TAG/merged_16bit" ] || { say "!! TRAIN FAILED $TAG — no merged_16bit"; return 1; }
  say "### TRAIN DONE $TAG ###"
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  bash serve_any.sh "$TAG" 2>&1
  say "### EVAL $TAG docs=none rep=3 ###"
  bash run_eval_v2.sh "$TAG" none 3 2>&1
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  say "### RUN COMPLETE $TAG ###"
}

run_one q3ca3b-ml1x data_qwen_ml1x
run_one q3ca3b-ml2x data_qwen_ml2x

say ""
say "=== A3B SWEEP DONE ==="
for T in q3ca3b-ml1x q3ca3b-ml2x; do
  [ -f /tmp/${T}_v2_none.json ] && say "  $T eval json present" || say "  $T (no eval json)"
done
say "=== A3B SWEEP END ==="
