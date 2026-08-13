#!/usr/bin/env bash
# The rea test corpus moved into the rea submodule (components/rea/tests)
# as part of the repo split, giving it one home in the rea repo. This shim runs
# that corpus against the umbrella-built binary so CI and the rea_tests CTest
# target keep working over the same fixtures. Env (e.g. REA_SKIP_TESTS) passes through.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec env REA_BIN="$ROOT_DIR/build/bin/rea" bash "$ROOT_DIR/components/rea/tests/run.sh" "$@"
