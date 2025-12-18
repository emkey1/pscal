#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT="${BUILD_DIR}/ios_vproc_test"
OUT_PGID="${BUILD_DIR}/ios_vproc_pgid_test"
OUT_SHIMS="${BUILD_DIR}/ios_vproc_signal_shim_test"
OUT_JOBCTRL="${BUILD_DIR}/ios_vproc_jobcontrol_shim_test"

mkdir -p "${BUILD_DIR}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   -Isrc -I"${ROOT}" \
   "${ROOT}/Tests/ios_vproc/test_vproc.c" \
   "${ROOT}/src/ios/vproc.c" \
   "${ROOT}/src/common/path_virtualization.c" \
   "${ROOT}/src/common/path_truncate.c" \
   -o "${OUT}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   -Isrc -I"${ROOT}" \
   "${ROOT}/Tests/ios_vproc/test_pgid_sid.c" \
   "${ROOT}/src/ios/vproc.c" \
   "${ROOT}/src/common/path_virtualization.c" \
   "${ROOT}/src/common/path_truncate.c" \
   -o "${OUT_PGID}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   -Isrc -I"${ROOT}" \
   "${ROOT}/Tests/ios_vproc/test_signal_shims.c" \
   "${ROOT}/src/ios/vproc.c" \
   "${ROOT}/src/common/path_virtualization.c" \
   "${ROOT}/src/common/path_truncate.c" \
   -o "${OUT_SHIMS}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   -Isrc -I"${ROOT}" \
   "${ROOT}/Tests/ios_vproc/test_jobcontrol_shims.c" \
   "${ROOT}/src/ios/vproc.c" \
   "${ROOT}/src/common/path_virtualization.c" \
   "${ROOT}/src/common/path_truncate.c" \
   -o "${OUT_JOBCTRL}"

echo "Running ios_vproc tests..."
"${OUT}"
"${OUT_PGID}"
"${OUT_SHIMS}"
"${OUT_JOBCTRL}"
