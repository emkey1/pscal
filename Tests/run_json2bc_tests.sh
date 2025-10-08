#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"
CLIKE_ARGS=(--no-cache)
JSON2BC_BIN="$ROOT_DIR/build/bin/pscaljson2bc"
PSCALVM_BIN="$ROOT_DIR/build/bin/pscalvm"

. "$SCRIPT_DIR/tools/harness_utils.sh"
harness_init

strip_ansi_inplace() {
    local path="$1"
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

run_json2bc_fixture() {
    local src="$1"
    local test_name="$2"
    local stdout_expect="$SCRIPT_DIR/json2bc/$test_name.out"

    local actual_out=$(mktemp)
    local actual_err=$(mktemp)
    local tmp_bc=$(mktemp)

    local details=()
    local status="PASS"

    set +e
    "$CLIKE_BIN" "${CLIKE_ARGS[@]}" --dump-ast-json "$src" | "$JSON2BC_BIN" -o "$tmp_bc" > "$actual_err" 2>&1
    local compile_status=$?
    set -e
    if [ $compile_status -ne 0 ]; then
        status="FAIL"
        details+=("clike | json2bc pipeline exited with $compile_status")
        if [ -s "$actual_err" ]; then
            details+=("pipeline output:\n$(cat "$actual_err")")
        fi
    else
        "$PSCALVM_BIN" "$tmp_bc" > "$actual_out" 2>> "$actual_err"
        local vm_status=$?
        strip_ansi_inplace "$actual_out"
        strip_ansi_inplace "$actual_err"

        if [ -f "$stdout_expect" ]; then
            set +e
            diff_output=$(diff -u "$stdout_expect" "$actual_out")
            local diff_status=$?
            set -e
            if [ $diff_status -ne 0 ]; then
                status="FAIL"
                if [ $diff_status -eq 1 ]; then
                    details+=("stdout mismatch:\n$diff_output")
                else
                    details+=("diff on stdout failed with status $diff_status")
                fi
            fi
        elif [ -s "$actual_out" ]; then
            status="FAIL"
            details+=("Unexpected stdout:\n$(cat "$actual_out")")
        fi

        if [ $vm_status -ne 0 ]; then
            status="FAIL"
            details+=("pscalvm exited with $vm_status")
        fi

        if [ -s "$actual_err" ]; then
            status="FAIL"
            details+=("Unexpected stderr:\n$(cat "$actual_err")")
        fi
    fi

    rm -f "$tmp_bc" "$actual_out" "$actual_err"

    if [ "$status" = "PASS" ]; then
        harness_report PASS "json2bc_${test_name}" "json2bc fixture $test_name"
    else
        harness_report FAIL "json2bc_${test_name}" "json2bc fixture $test_name" "${details[@]}"
    fi
}

if [ ! -x "$CLIKE_BIN" ]; then
    echo "clike binary not found at $CLIKE_BIN" >&2
    exit 1
fi
if [ ! -x "$JSON2BC_BIN" ]; then
    echo "pscaljson2bc binary not found at $JSON2BC_BIN" >&2
    exit 1
fi
if [ ! -x "$PSCALVM_BIN" ]; then
    echo "pscalvm binary not found at $PSCALVM_BIN" >&2
    exit 1
fi

TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"
trap 'rm -rf "$TEST_HOME"' EXIT

shopt -s nullglob
for src in "$SCRIPT_DIR"/json2bc/*.cl; do
    test_name=$(basename "$src" .cl)
    run_json2bc_fixture "$src" "$test_name"
done
shopt -u nullglob

harness_summary "json2bc"
if harness_exit_code; then
    exit 0
fi
exit 1
