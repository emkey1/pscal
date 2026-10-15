#!/bin/bash
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
TAG=qwen35-9b-cs-aug4
MODEL=/storage/models/Qwen3.5-9B
MERGED=/storage/$TAG/merged_16bit
mkdir -p /storage/$TAG
docker rm -f aether-train-$TAG >/dev/null 2>&1
docker run -d --name aether-train-$TAG --gpus all --ipc=host --shm-size=16g   --ulimit memlock=-1 --ulimit stack=67108864   -v $W:/workspace -v /storage:/storage -e HF_HOME=/storage/hf   aether-unsloth-qwen3-coder-30b:base26-fla   python /workspace/scripts/unsloth_qwen_coder_30b_sft.py     --model-id $MODEL     --instruction-jsonl /workspace/data_cs_aug4/aether_instruction_sft.jsonl     --repair-jsonl /workspace/data_cs_aug4/aether_repair_sft.jsonl     --output-dir /workspace/runs/sft-$TAG     --epochs 3 --lora-r 32 --lora-alpha 64     --learning-rate 1e-4 --batch-size 1 --grad-accum 8     --export-merged-16bit --merged-output-dir $MERGED
echo LAUNCHED
