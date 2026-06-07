#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(os.path.dirname(sys.argv[1])))' "${BASH_SOURCE[0]}")"
ROOT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$SCRIPT_DIR/..")"
AETHER_BIN="$ROOT_DIR/build/bin/aether"
SMOKE_FIXTURE="$ROOT_DIR/Tests/aether/smoke.aether"
CONTRACT_PASS_FIXTURE="$ROOT_DIR/Tests/aether/contracts_pass.aether"
CONTRACT_FAIL_PRE_FIXTURE="$ROOT_DIR/Tests/aether/contracts_fail_pre.aether"
CONTRACT_FAIL_POST_FIXTURE="$ROOT_DIR/Tests/aether/contracts_fail_post.aether"
EFFECTS_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/effects_fail_outside_fx.aether"
PURE_PASS_FIXTURE="$ROOT_DIR/Tests/aether/pure_pass.aether"
PURE_FAIL_EFFECTFUL_FIXTURE="$ROOT_DIR/Tests/aether/pure_fail_effectful.aether"
PURE_FAIL_NON_PURE_CALL_FIXTURE="$ROOT_DIR/Tests/aether/pure_fail_non_pure_call.aether"

if [ ! -x "$AETHER_BIN" ]; then
    echo "missing aether binary: $AETHER_BIN" >&2
    exit 1
fi

for fixture in \
    "$SMOKE_FIXTURE" \
    "$CONTRACT_PASS_FIXTURE" \
    "$CONTRACT_FAIL_PRE_FIXTURE" \
    "$CONTRACT_FAIL_POST_FIXTURE" \
    "$EFFECTS_FAIL_FIXTURE" \
    "$PURE_PASS_FIXTURE" \
    "$PURE_FAIL_EFFECTFUL_FIXTURE" \
    "$PURE_FAIL_NON_PURE_CALL_FIXTURE"
do
    if [ ! -f "$fixture" ]; then
        echo "missing fixture: $fixture" >&2
        exit 1
    fi
done

"$AETHER_BIN" --no-cache --no-run "$SMOKE_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache --dump-ast-json "$SMOKE_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$CONTRACT_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$PURE_PASS_FIXTURE" >/dev/null

if "$AETHER_BIN" --no-cache "$CONTRACT_FAIL_PRE_FIXTURE" >/tmp/aether_contract_fail_pre.out 2>&1; then
    echo "expected precondition failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether @pre failed in inc" /tmp/aether_contract_fail_pre.out; then
    echo "missing precondition failure message" >&2
    cat /tmp/aether_contract_fail_pre.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_FAIL_POST_FIXTURE" >/tmp/aether_contract_fail_post.out 2>&1; then
    echo "expected postcondition failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether @post failed in inc" /tmp/aether_contract_fail_post.out; then
    echo "missing postcondition failure message" >&2
    cat /tmp/aether_contract_fail_post.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$EFFECTS_FAIL_FIXTURE" >/tmp/aether_effects_fail.out 2>&1; then
    echo "expected effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'writeln' requires an fx block" /tmp/aether_effects_fail.out; then
    echo "missing effect-boundary failure message" >&2
    cat /tmp/aether_effects_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PURE_FAIL_EFFECTFUL_FIXTURE" >/tmp/aether_pure_fail_effectful.out 2>&1; then
    echo "expected purity failure for effectful builtin but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether purity error: pure function 'noisy' cannot call effectful builtin 'writeln'" /tmp/aether_pure_fail_effectful.out; then
    echo "missing purity failure for effectful builtin" >&2
    cat /tmp/aether_pure_fail_effectful.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PURE_FAIL_NON_PURE_CALL_FIXTURE" >/tmp/aether_pure_fail_non_pure_call.out 2>&1; then
    echo "expected purity failure for non-pure call but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether purity error: pure function 'wrapper' cannot call non-pure function 'noisy'" /tmp/aether_pure_fail_non_pure_call.out; then
    echo "missing purity failure for non-pure call" >&2
    cat /tmp/aether_pure_fail_non_pure_call.out >&2
    exit 1
fi

echo "aether smoke tests passed"
