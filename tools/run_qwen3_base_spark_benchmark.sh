#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output_json="${1:-$repo_root/Tests/aether_doc_bench/out/qwen3_4b_base_baseline.json}"
adapter_path="${2:-}"

mkdir -p "$(dirname "$output_json")"

# The Spark host is private: $PSCAL_SPARK_HOST, or the fleet overlay.
FLEET_ENV="${PSCAL_FLEET_ENV:-$HOME/.config/pscal/fleet.env}"
if [ -z "${PSCAL_SPARK_HOST:-}" ] && [ -r "$FLEET_ENV" ]; then . "$FLEET_ENV"; fi
spark_host="${PSCAL_SPARK_HOST:?set PSCAL_SPARK_HOST (or add it to ~/.config/pscal/fleet.env)}"

start_args=(python3 "$repo_root/tools/spark_qwen3_base_remote.py" --host "$spark_host" start-server --wait-seconds 1800)
if [ -n "$adapter_path" ]; then
  start_args=(python3 "$repo_root/tools/spark_qwen3_base_remote.py" --host "$spark_host" --adapter-path "$adapter_path" start-server --wait-seconds 1800)
fi
"${start_args[@]}"

python3 "$repo_root/tools/aether_doc_bench.py" \
  --destinations-config "$repo_root/Tests/aether_doc_bench/does_not_exist.local.json" \
  --provider command \
  --command-template "python3 $repo_root/tools/spark_qwen3_base_remote.py generate --prompt-file {prompt_file} --max-new-tokens 3000 --timeout-seconds 900" \
  --docs full,small,none \
  --python-baseline \
  --text-summary \
  --output-json "$output_json"
