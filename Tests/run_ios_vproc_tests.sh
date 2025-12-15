#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT="${BUILD_DIR}/ios_vproc_test"

mkdir -p "${BUILD_DIR}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   -Isrc -I"${ROOT}" \
   "${ROOT}/Tests/ios_vproc/test_vproc.c" \
   "${ROOT}/src/ios/vproc.c" \
   "${ROOT}/src/common/path_virtualization.c" \
   "${ROOT}/src/common/path_truncate.c" \
   -o "${OUT}"

echo "Running ios_vproc tests..."
"${OUT}"
