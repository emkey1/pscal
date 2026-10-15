#!/bin/bash
# usage: serve_v77.sh <tag>   -> serves /storage/aether-qwen25-coder-7b-<tag>/merged_16bit on :8019
set -u
TAG=${1:-v77}
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
  -v /storage:/storage -p 8019:8000 \
  vllm/vllm-openai:latest \
  --model /storage/aether-qwen25-coder-7b-$TAG/merged_16bit \
  --served-model-name aether-qwen25-coder-7b-$TAG \
  --dtype bfloat16 --max-model-len 16384 --gpu-memory-utilization 0.85 --enforce-eager --port 8000
echo "serving aether-qwen25-coder-7b-$TAG : $(docker ps -q -f name=aether-bench-vllm-q7)"
