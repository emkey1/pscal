#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT="${BUILD_DIR}/ios_openssh_shim_fd_lifecycle_test"
INCLUDE_DIRS=(
  "-I${ROOT}/src"
  "-I${ROOT}"
)

mkdir -p "${BUILD_DIR}"

VPROC_SOURCES=(
  "${ROOT}/src/ios/vproc.c"
  "${ROOT}/src/common/path_virtualization.c"
  "${ROOT}/src/common/path_truncate.c"
  "${ROOT}/src/common/runtime_tty.c"
  "${ROOT}/src/ios/runtime_session_stub.c"
  "${ROOT}/src/ios/tty/ish_compat.c"
  "${ROOT}/src/ios/tty/pscal_fd.c"
  "${ROOT}/src/ios/tty/pscal_tty.c"
  "${ROOT}/src/ios/tty/pscal_pty.c"
  "${ROOT}/src/ios/tty/pscal_tty_host.c"
  "${ROOT}/Tests/ios_vproc/test_smallclue_applets_stub.c"
)

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_pscal_ios_shim_fd_lifecycle.c" \
   "${ROOT}/third-party/openssh-10.2p1/pscal_ios_shim.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT}"

echo "Running iOS OpenSSH shim FD lifecycle tests..."
"${OUT}"
