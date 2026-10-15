#!/bin/bash
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
cd "$W" || exit 2
M14B=$(ls -d /storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/*/ | head -1)
say(){ echo "$(date +%H:%M:%S) $*"; }
TAG=qwen14b-ml3x
say "=== ML3X START (14B r64/a128/4ep data_qwen_ml3x) ==="
say "model: $M14B"
docker rm -f aether-train-$TAG >/dev/null 2>&1
bash train_any.sh "$TAG" "$M14B" 64 128 data_qwen_ml3x 4 2>&1
say "### waiting on aether-train-$TAG ###"
docker wait aether-train-$TAG 2>&1
docker logs --tail 12 aether-train-$TAG 2>&1
if [ ! -d /storage/$TAG/merged_16bit ]; then say "!! TRAIN FAILED $TAG"; exit 1; fi
say "### TRAIN DONE $TAG ###"
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
bash serve_any.sh "$TAG" 2>&1
say "### EVAL $TAG none rep=3 ###"
bash run_eval_v2.sh "$TAG" none 3 2>&1
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
say "### REEVAL DONE qwen14b-ml3x ###"
say "=== ML3X END ==="
