#!/usr/bin/env bash
# The clike test corpus moved into the clike submodule (components/clike/tests)
# as part of the repo split, giving it one home in the clike repo. This shim runs
# that corpus against the umbrella-built binary so CI and the clike_tests CTest
# target keep working over the same fixtures. Env (e.g. REA_SKIP_TESTS) passes through.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec env CLIKE_BIN="$ROOT_DIR/build/bin/clike" bash "$ROOT_DIR/components/clike/tests/run.sh" "$@"
