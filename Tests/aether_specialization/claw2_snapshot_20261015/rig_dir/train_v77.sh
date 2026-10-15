#!/bin/bash
# usage: train_v77.sh <tag> <lora_r> <lora_alpha> [epochs] [data_dir]
set -u
TAG=${1:-v77}; R=${2:-16}; ALPHA=${3:-32}; EP=${4:-3}; DATA=${5:-data_v77}
W=$HOME/training/aether-qwen-coder-30b-unsloth
docker rm -f aether-train-$TAG >/dev/null 2>&1
echo "launching aether-train-$TAG  r=$R alpha=$ALPHA epochs=$EP data=$DATA"
docker run -d --name aether-train-$TAG --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v $W:/workspace -v /storage:/storage \
  aether-unsloth-qwen3-coder-30b:q3 \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /workspace/model-mirror \
    --instruction-jsonl /workspace/$DATA/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/$DATA/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-qwen25-coder-7b-$TAG \
    --no-load-in-4bit --epochs $EP --lora-r $R --lora-alpha $ALPHA \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit \
    --merged-output-dir /storage/aether-qwen25-coder-7b-$TAG/merged_16bit
echo "container: $(docker ps -q -f name=aether-train-$TAG)"
