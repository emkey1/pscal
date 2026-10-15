#!/bin/bash
set -u
TAG=$1; MAXLEN=${2:-16384}; GPUMEM=${3:-0.85}
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
  -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
  --model /storage/$TAG/merged_16bit --served-model-name $TAG \
  --dtype bfloat16 --max-model-len $MAXLEN --gpu-memory-utilization $GPUMEM --enforce-eager --port 8000
echo "serving $TAG (gpu-memory-utilization=$GPUMEM)"
