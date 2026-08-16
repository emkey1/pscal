#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT="${BUILD_DIR}/ios_vproc_test"
OUT_PGID="${BUILD_DIR}/ios_vproc_pgid_test"
OUT_SHIMS="${BUILD_DIR}/ios_vproc_signal_shim_test"
OUT_JOBCTRL="${BUILD_DIR}/ios_vproc_jobcontrol_shim_test"
OUT_RESIZE="${BUILD_DIR}/ios_vproc_resize_harness_test"
OUT_NEXTVI_PASTE="${BUILD_DIR}/ios_nextvi_bracketed_paste_test"
NEXTVI_OBJ="${BUILD_DIR}/nextvi_ios_regression_vi.o"
# The vproc/tty platform layer lives in pscal-core since the components split;
# only the back-compat headers and the session stub stayed in the umbrella's src/ios.
CORE="${ROOT}/components/pscal-core"
INCLUDE_DIRS=(
  "-I${ROOT}/src"
  "-I${ROOT}"
  "-I${CORE}/src"
  "-I${CORE}"
)

mkdir -p "${BUILD_DIR}"

VPROC_SOURCES=(
  "${CORE}/src/runtime/vproc/vproc.c"
  "${CORE}/src/common/path_virtualization.c"
  "${CORE}/src/common/path_truncate.c"
  "${CORE}/src/common/runtime_tty.c"
  "${ROOT}/src/ios/runtime_session_stub.c"
  "${CORE}/src/runtime/vproc/tty/ish_compat.c"
  "${CORE}/src/runtime/vproc/tty/pscal_fd.c"
  "${CORE}/src/runtime/vproc/tty/pscal_tty.c"
  "${CORE}/src/runtime/vproc/tty/pscal_pty.c"
  "${CORE}/src/runtime/vproc/tty/pscal_tty_host.c"
  "${ROOT}/Tests/ios_vproc/test_smallclue_applets_stub.c"
)

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_vproc.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_pgid_sid.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_PGID}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_signal_shims.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_SHIMS}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_jobcontrol_shims.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_JOBCTRL}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_resize_harness.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_RESIZE}"

cc -DPSCAL_TARGET_IOS=1 \
   -DNEXTVI_REGRESSION_TEST_HOOKS=1 \
   -Dmain=nextvi_main_entry \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   -c "${CORE}/third-party/nextvi/vi.c" \
   -o "${NEXTVI_OBJ}"

cc -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_nextvi_bracketed_paste_regression.c" \
   "${NEXTVI_OBJ}" \
   -o "${OUT_NEXTVI_PASTE}"

echo "Running ios_vproc tests..."
"${OUT}"
"${OUT_PGID}"
"${OUT_SHIMS}"
"${OUT_JOBCTRL}"
"${OUT_RESIZE}"
"${OUT_NEXTVI_PASTE}"
