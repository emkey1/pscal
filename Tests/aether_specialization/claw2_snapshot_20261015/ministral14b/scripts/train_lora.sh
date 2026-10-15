#!/usr/bin/env bash
set -euo pipefail
CONFIG="${1:-/workspace/configs/lora_8k.yaml}"
MODEL_DIR="$HOME/models/hf/mistralai/Ministral-3-14B-Reasoning-2512"
DATA_DIR="$HOME/datasets/openclaw-sft-v0"
WORK_DIR="$HOME/training/ministral14b"
mkdir -p "$DATA_DIR" "$WORK_DIR/outputs" "$WORK_DIR/logs"
if [[ ! -d "$MODEL_DIR" ]]; then
  echo "Missing model dir: $MODEL_DIR" >&2
  echo "Run scripts/download_ministral14b.sh first." >&2
  exit 2
fi
if [[ ! -s "$DATA_DIR/train.jsonl" ]]; then
  echo "Missing dataset: $DATA_DIR/train.jsonl" >&2
  exit 3
fi
if [[ ! -s "$DATA_DIR/eval.jsonl" ]]; then
  echo "Missing dataset: $DATA_DIR/eval.jsonl" >&2
  exit 3
fi
# Stop serving containers if explicitly requested; training competes for unified GPU memory.
if [[ "${STOP_QWEN_FOR_TRAINING:-0}" == "1" ]]; then
  docker stop openclaw-qwen-moe-vllm >/dev/null 2>&1 || true
fi
docker run --rm --gpus all --ipc=host --ulimit memlock=-1 --ulimit stack=67108864 \
  -v "$MODEL_DIR":/models/Ministral-3-14B-Reasoning-2512:ro \
  -v "$DATA_DIR":/datasets/openclaw-sft-v0:ro \
  -v "$WORK_DIR":/workspace \
  --entrypoint python3 \
  openclaw-ministral14b-train:latest \
  /workspace/src/train_lora.py --config "$CONFIG"
