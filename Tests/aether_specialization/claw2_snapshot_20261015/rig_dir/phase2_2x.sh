#!/bin/bash
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
cd "$W" || exit 2
BASE=$(ls -d /storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/*/ | head -1)
TAG=qwen3-8b-think-2x
say(){ echo "$(date +%H:%M:%S) $*"; }
say "=== PHASE2-2X START: trace-gen(2x) -> thinking-SFT -> dual eval @0.6 ==="
# 1) serve base Qwen3-8B for trace generation
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
  -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
  --model "$BASE" --served-model-name qwen3-8b-base \
  --dtype bfloat16 --max-model-len 8192 --gpu-memory-utilization 0.85 --enforce-eager --port 8000 >/dev/null
for i in $(seq 1 90); do curl -s --max-time 5 http://localhost:8019/v1/models 2>/dev/null | grep -q qwen3-8b-base && { say VLLM_READY_base; break; }; sleep 10; done
say "### GEN TRACES (data_qwen_ml2x) ###"
python3 -u "$W/gen_traces.py" data_qwen_ml2x data_qwen_ml2x_think 2>&1
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
# 2) thinking-SFT on the 2x traces (same recipe as the 1x thinking run)
say "### TRAIN $TAG (data_qwen_ml2x_think r32/a64/3ep) ###"
docker rm -f aether-train-$TAG >/dev/null 2>&1
bash train_any.sh "$TAG" "$BASE" 32 64 data_qwen_ml2x_think 3 2>&1
docker wait aether-train-$TAG 2>&1
docker logs --tail 10 aether-train-$TAG 2>&1
if [ ! -d /storage/$TAG/merged_16bit ]; then say "!! TRAIN FAILED $TAG"; exit 1; fi
say "### TRAIN DONE $TAG ###"
# 3) serve trained + dual eval at proper thinking sampling
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
  -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
  --model /storage/$TAG/merged_16bit --served-model-name $TAG \
  --dtype bfloat16 --max-model-len 16384 --gpu-memory-utilization 0.85 --enforce-eager --port 8000 >/dev/null
say "### EVAL think @0.6 ###"; bash $HOME/pscal-bench/run_eval_think.sh "$TAG" think 4000 0.6 2>&1
say "### EVAL nothink @0.6 ###"; bash $HOME/pscal-bench/run_eval_think.sh "$TAG" nothink 4000 0.6 2>&1
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
say "### PHASE2-2X DONE qwen3-8b-think-2x ###"
