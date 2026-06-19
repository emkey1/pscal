#!/usr/bin/env bash
# The pascal test corpus moved into the pascal submodule (components/pascal/tests)
# as part of the repo split, giving it one home in the pascal repo. This shim runs
# that corpus against the umbrella-built binary so CI and the pascal_tests CTest
# target keep working over the same fixtures. Env (e.g. REA_SKIP_TESTS) passes through.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec env PASCAL_BIN="$ROOT_DIR/build/bin/pascal" bash "$ROOT_DIR/components/pascal/tests/run.sh" "$@"
