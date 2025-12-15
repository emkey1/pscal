#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${ROOT}/build/bin/exsh"

if [[ ! -x "${BIN}" ]]; then
  echo "exsh binary not found at ${BIN}" >&2
  exit 77
fi

run_exsh() {
  local script="$1"
  "${BIN}" --norc --noprofile <<'EOF'
set -e
PS4='+ '
set -m
EOF
}

echo "TODO: shell-level jobspec parse tests run against exsh binary on device."
