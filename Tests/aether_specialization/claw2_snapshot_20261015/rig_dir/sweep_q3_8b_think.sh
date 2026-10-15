#!/bin/bash
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
cd "$W" || exit 2
M=$(ls -d /storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/*/ | head -1)
say(){ echo "$(date +%H:%M:%S) $*"; }
TAG=qwen3-8b-think
say "=== Q3-8B THINK-SFT START (data_qwen_ml1x_think, r32/a64/3ep) ==="
docker rm -f aether-train-$TAG >/dev/null 2>&1
bash train_any.sh "$TAG" "$M" 32 64 data_qwen_ml1x_think 3 2>&1
say "### waiting on aether-train-$TAG ###"
docker wait aether-train-$TAG 2>&1
docker logs --tail 12 aether-train-$TAG 2>&1
if [ ! -d /storage/$TAG/merged_16bit ]; then say "!! TRAIN FAILED $TAG"; exit 1; fi
say "### TRAIN DONE $TAG ###"
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
  -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
  --model /storage/$TAG/merged_16bit --served-model-name $TAG \
  --dtype bfloat16 --max-model-len 16384 --gpu-memory-utilization 0.85 --enforce-eager --port 8000 >/dev/null
say "### EVAL think ###"
bash $HOME/pscal-bench/run_eval_think.sh "$TAG" think 4000 2>&1
say "### EVAL nothink ###"
bash $HOME/pscal-bench/run_eval_think.sh "$TAG" nothink 4000 2>&1
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
say "### PHASE2 DONE qwen3-8b-think ###"
say "=== Q3-8B THINK-SFT END ==="
