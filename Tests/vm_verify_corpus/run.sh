#!/usr/bin/env bash
# Regenerates and runs the Phase 1e verifier's malformed-.bc corpus
# (Docs/pscal_vm2_plan.md §5.5). Requires build/bin/pascal and
# build/bin/pscalvm to already be built.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PYTHON:-python3}"

"$PY" "$SCRIPT_DIR/generate_corpus.py" "$@" || exit 1
"$PY" "$SCRIPT_DIR/run_corpus_tests.py" || exit 1
"$PY" "$SCRIPT_DIR/test_stack_ceiling.py"
