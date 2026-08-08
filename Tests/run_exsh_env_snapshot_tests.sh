#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT="${BUILD_DIR}/exsh_env_snapshot_restore_test"

mkdir -p "${BUILD_DIR}"

cc -std=c11 -pthread \
  -I"${ROOT}/components/pscal-core/src" \
  -I"${ROOT}" \
  "${ROOT}/components/exsh/tests/test_env_snapshot_restore.c" \
  "${ROOT}/components/pscal-core/src/common/env_snapshot.c" \
  -o "${OUT}"

"${OUT}"
