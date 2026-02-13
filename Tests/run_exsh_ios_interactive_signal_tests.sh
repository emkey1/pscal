#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build/ios-host"
EXE="${BUILD_DIR}/bin/exsh"

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DPSCAL_FORCE_IOS=ON \
  -DVPROC_ENABLE_STUBS_FOR_TESTS=ON \
  -DPSCAL_NO_CLI_ENTRYPOINTS=OFF \
  -DPSCAL_BUILD_STATIC_LIBS=ON \
  -DSDL=OFF \
  -DPSCAL_USE_BUNDLED_CURL=OFF

cmake --build "${BUILD_DIR}" --target exsh

RUN_VPROC_TESTS=1 python3 "${ROOT}/Tests/exsh/exsh_interactive_test_harness.py" --executable "${EXE}" "$@"
