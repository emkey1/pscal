#!/bin/bash
# Dense scaling-line extension: Mistral-Small-24B-Instruct-2501 (different family) x {1x,2x corpus}.
# Recipe = the strongest dense recipe (14B: r64/a128/4ep), held constant; only corpus varies.
# Masking: Mistral [INST]/[/INST] (single tokens 3/4, response fuses w/ no space).
# Datasets reused as-is (records are model-agnostic messages; tokenizer renders Mistral format).
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
cd "$W" || exit 2
export INSTR_PART="[INST]"
export RESP_PART="[/INST]"
MMODEL=$(ls -d /storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/*/ 2>/dev/null | head -1)
say() { echo "$(date "+%H:%M:%S") $*"; }
say "=== MISTRAL SWEEP START ==="
say "model: $MMODEL"
say "mask:  INSTR=$INSTR_PART RESP=$RESP_PART"
[ -d "$MMODEL" ] || { say "!! missing Mistral snapshot; abort"; exit 3; }

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
  docker logs --tail 15 "aether-train-$TAG" 2>&1
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

run_one mistral24b-ml1x "$MMODEL" 64 128 data_qwen_ml1x 4
run_one mistral24b-ml2x "$MMODEL" 64 128 data_qwen_ml2x 4

say ""
say "=== MISTRAL SWEEP DONE ==="
for T in mistral24b-ml1x mistral24b-ml2x; do
  J=/tmp/${T}_v2_none.json
  [ -f "$J" ] && say "  $T eval json present: $J" || say "  $T : (no eval json)"
done
say "=== MISTRAL SWEEP END ==="
