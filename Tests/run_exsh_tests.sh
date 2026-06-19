#!/usr/bin/env bash
# The exsh test corpus moved into the exsh submodule (components/exsh/tests)
# as part of the repo split, giving it one home in the exsh repo. This shim runs
# that corpus against the umbrella-built binary so CI and the exsh_tests CTest
# target keep working over the same fixtures. Env (e.g. REA_SKIP_TESTS) passes through.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec env EXSH_BIN="$ROOT_DIR/build/bin/exsh" bash "$ROOT_DIR/components/exsh/tests/run.sh" "$@"
