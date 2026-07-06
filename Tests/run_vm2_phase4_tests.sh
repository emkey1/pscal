#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PSCAL_CORE_SRC="${ROOT}/components/pscal-core/src"
BUILD_DIR="${ROOT}/build"
OUT="${BUILD_DIR}/vm2_phase4_obj_header_test"

mkdir -p "${BUILD_DIR}"

cc -std=c11 -Wall -Wextra \
   -I"${PSCAL_CORE_SRC}" \
   "${ROOT}/Tests/vm2_phase4/test_obj_header.c" \
   "${PSCAL_CORE_SRC}/core/obj_header.c" \
   -o "${OUT}"

echo "Running VM 2.0 Phase 4a unit tests..."
"${OUT}"
