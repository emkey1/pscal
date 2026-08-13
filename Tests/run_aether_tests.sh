#!/usr/bin/env bash
# The Aether test corpus moved into the aether submodule (components/aether/tests)
# as part of the repo split, so it now has a single home in the aether repo. This
# shim runs that corpus against the umbrella-built binary, keeping the umbrella's
# CTest registration (add_test aether_tests) working over the same fixtures.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec env AETHER_BIN="$ROOT_DIR/build/bin/aether" \
    bash "$ROOT_DIR/components/aether/tests/run.sh"
