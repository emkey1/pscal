#!/bin/bash
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
BASE=$(ls -d /storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/*/ | head -1)
say(){ echo "$(date +%H:%M:%S) $*"; }
say "=== PHASE2 TRACE-GEN START (base Qwen3-8B rationalizes 1x corpus) ==="
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
  -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
  --model "$BASE" --served-model-name qwen3-8b-base \
  --dtype bfloat16 --max-model-len 8192 --gpu-memory-utilization 0.85 --enforce-eager --port 8000 >/dev/null
for i in $(seq 1 90); do curl -s --max-time 5 http://localhost:8019/v1/models 2>/dev/null | grep -q qwen3-8b-base && { say VLLM_READY; break; }; sleep 10; done
python3 -u $W/gen_traces.py 2>&1
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
say "=== PHASE2 TRACE-GEN END ==="
