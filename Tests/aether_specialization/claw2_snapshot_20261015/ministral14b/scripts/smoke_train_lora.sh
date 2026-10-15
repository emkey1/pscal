#!/usr/bin/env bash
set -euo pipefail
MODEL_DIR="$HOME/models/hf/mistralai/Ministral-3-14B-Reasoning-2512"
DATA_DIR="$HOME/datasets/openclaw-sft-v0"
WORK_DIR="$HOME/training/ministral14b"
mkdir -p "$WORK_DIR/logs" "$WORK_DIR/outputs"
if [[ ! -s "$DATA_DIR/train.jsonl" || ! -s "$DATA_DIR/eval.jsonl" ]]; then
  echo "Missing real dataset under $DATA_DIR; refusing to overwrite it with examples." >&2
  exit 3
fi
# Tiny smoke: same 8K config, real dataset, only 1 optimizer step.
# Training competes for unified GPU memory; stop/restart Qwen only when explicitly requested.
if [[ "${STOP_QWEN_FOR_TRAINING:-0}" == "1" ]]; then
  docker stop openclaw-qwen-moe-vllm >/dev/null 2>&1 || true
  trap 'docker start openclaw-qwen-moe-vllm >/dev/null 2>&1 || true' EXIT
fi
docker run --rm --gpus all --ipc=host --ulimit memlock=-1 --ulimit stack=67108864 \
  -e OPENCLAW_TRAIN_MAX_STEPS="${OPENCLAW_TRAIN_MAX_STEPS:-1}" \
  -v "$MODEL_DIR":/models/Ministral-3-14B-Reasoning-2512:ro \
  -v "$DATA_DIR":/datasets/openclaw-sft-v0:ro \
  -v "$WORK_DIR":/workspace \
  --entrypoint python3 \
  openclaw-ministral14b-train:latest \
  /workspace/src/train_lora.py --config /workspace/configs/lora_8k.yaml
