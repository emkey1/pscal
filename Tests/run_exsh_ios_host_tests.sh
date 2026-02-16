#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build/ios-host"
EXE="${BUILD_DIR}/bin/exsh"

# Configure (or reconfigure) the iOS-flavored host build.
cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DPSCAL_FORCE_IOS=ON \
  -DVPROC_ENABLE_STUBS_FOR_TESTS=ON \
  -DPSCAL_NO_CLI_ENTRYPOINTS=OFF \
  -DPSCAL_BUILD_STATIC_LIBS=ON \
  -DSDL=OFF \
  -DPSCAL_USE_BUNDLED_CURL=OFF

# Build the exsh binary that exercises the vproc path.
cmake --build "${BUILD_DIR}" --target exsh

# Run the targeted jobspec regression against the iOS-flavored binary.
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only jobspec
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only jobs_wait_all
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only jobspec_kill
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only watch_top_vproc
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only watch_top_foreground_vproc
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only lps_shell_label
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only top_tree_default
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only lps_tree_option
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only kernel_tree_root
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only pipeline_help_grep_ios
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only pipeline_empty_ios
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only pipeline_clike_head_ios
RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_test_harness.py" --executable "${EXE}" --only pipeline_rea_head_ios

# Optional interactive PTY signal regressions. Enable explicitly while
# debugging local Ctrl-C/Ctrl-Z behavior:
#   RUN_INTERACTIVE_SIGNAL_TESTS=1 Tests/run_exsh_ios_host_tests.sh
if [ "${RUN_INTERACTIVE_SIGNAL_TESTS:-0}" = "1" ]; then
  RUN_VPROC_TESTS=1 python "${ROOT}/Tests/exsh/exsh_interactive_test_harness.py" --executable "${EXE}"
fi
