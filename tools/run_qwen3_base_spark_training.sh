#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
run_name="${1:-sft-seed-v1}"

python3 "$repo_root/tools/aether_specialization_sync_to_spark.py"
python3 "$repo_root/tools/spark_qwen3_base_train_remote.py" start --run-name "$run_name"
python3 "$repo_root/tools/spark_qwen3_base_train_remote.py" status --run-name "$run_name"
