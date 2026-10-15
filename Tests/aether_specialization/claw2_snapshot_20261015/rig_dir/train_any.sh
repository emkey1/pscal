#!/bin/bash
set -u
TAG=$1; MODEL=$2; R=${3:-32}; ALPHA=${4:-64}; DATA=${5:-data_v78}; EP=${6:-3}
W=$HOME/training/aether-qwen-coder-30b-unsloth
EXTRA=()
[ -n "${INSTR_PART:-}" ] && EXTRA+=(--instruction-part "${INSTR_PART}" --response-part "${RESP_PART}")
[ -n "${MAX_STEPS:-}" ] && EXTRA+=(--max-steps "${MAX_STEPS}")
docker rm -f aether-train-$TAG >/dev/null 2>&1
echo "train $TAG model=$MODEL r=$R a=$ALPHA data=$DATA ep=$EP extra='${EXTRA[*]-}'"
docker run -d --name aether-train-$TAG --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v $W:/workspace -v /storage:/storage -e HF_HOME=/storage/hf ${DOCKER_ENV:-} \
  ${IMG:-aether-unsloth-qwen3-coder-30b:q3} \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id "$MODEL" \
    --instruction-jsonl /workspace/$DATA/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/$DATA/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-$TAG \
    ${QUANT_FLAG-"--no-load-in-4bit"} --epochs $EP --lora-r $R --lora-alpha $ALPHA \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 ${EXTRA[@]+"${EXTRA[@]}"} \
    --export-merged-16bit --merged-output-dir /storage/$TAG/merged_16bit
echo "container: $(docker ps -q -f name=aether-train-$TAG)"
