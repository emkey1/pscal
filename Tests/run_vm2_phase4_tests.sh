#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PSCAL_CORE_SRC="${ROOT}/components/pscal-core/src"
BUILD_DIR="${ROOT}/build"
OBJ_HEADER_OUT="${BUILD_DIR}/vm2_phase4_obj_header_test"
TAGGED_WORD_OUT="${BUILD_DIR}/vm2_phase4_tagged_word_test"

mkdir -p "${BUILD_DIR}"

cc -std=c11 -Wall -Wextra \
   -I"${PSCAL_CORE_SRC}" \
   "${ROOT}/Tests/vm2_phase4/test_obj_header.c" \
   "${PSCAL_CORE_SRC}/core/obj_header.c" \
   -o "${OBJ_HEADER_OUT}"

echo "Running VM 2.0 Phase 4a unit tests..."
"${OBJ_HEADER_OUT}"

cc -std=c11 -Wall -Wextra \
   -I"${PSCAL_CORE_SRC}" \
   "${ROOT}/Tests/vm2_phase4/test_tagged_word.c" \
   "${PSCAL_CORE_SRC}/core/obj_header.c" \
   -o "${TAGGED_WORD_OUT}"

echo "Running VM 2.0 Phase 4h unit tests..."
"${TAGGED_WORD_OUT}"
