#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${ROOT}/build/bin/exsh"

if [[ ! -x "${BIN}" ]]; then
  echo "exsh binary not found at ${BIN}" >&2
  exit 77
fi

run_case() {
  local label="$1"
  local script="$2"
  echo "CASE: ${label}"
  "${BIN}" --norc --noprofile <<'EOF'
set -e
set -m
EOF
}

echo "NOTE: This script should be run on iOS with the in-app exsh binary available."
echo "TODO: Implement background job creation and %N resolution checks once device harness is wired."
