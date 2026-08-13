#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
run_name="${1:-sft-qwen-coder-30b-v1}"

python3 "$repo_root/tools/spark_unsloth_qwen_coder_train_remote.py" sync
python3 "$repo_root/tools/spark_unsloth_qwen_coder_train_remote.py" --run-name "$run_name" build-image
python3 "$repo_root/tools/spark_unsloth_qwen_coder_train_remote.py" --run-name "$run_name" start
python3 "$repo_root/tools/spark_unsloth_qwen_coder_train_remote.py" --run-name "$run_name" status
