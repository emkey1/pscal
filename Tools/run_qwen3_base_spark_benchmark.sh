#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output_json="${1:-$repo_root/Tests/aether_doc_bench/out/qwen3_4b_base_baseline.json}"

mkdir -p "$(dirname "$output_json")"

python3 "$repo_root/tools/spark_qwen3_base_remote.py" start-server --wait-seconds 1800

python3 "$repo_root/Tools/aether_doc_bench.py" \
  --destinations-config "$repo_root/Tests/aether_doc_bench/does_not_exist.local.json" \
  --provider command \
  --command-template "python3 $repo_root/tools/spark_qwen3_base_remote.py generate --prompt-file {prompt_file} --max-new-tokens 3000 --timeout-seconds 900" \
  --docs full,small,none \
  --python-baseline \
  --text-summary \
  --output-json "$output_json"
