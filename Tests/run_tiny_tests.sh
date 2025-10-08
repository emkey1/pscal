#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
TINY_BIN="$ROOT_DIR/tools/tiny"
VM_BIN="$ROOT_DIR/build/bin/pscalvm"
DCOMP_BIN="$ROOT_DIR/build/bin/pscald"

. "$SCRIPT_DIR/tools/harness_utils.sh"
harness_init

strip_ansi_inplace() {
    local path="$1"
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

run_tiny_fixture() {
    local src="$1"
    local test_name="$2"
    local input_file="$SCRIPT_DIR/tiny/$test_name.in"
    local stdout_expect="$SCRIPT_DIR/tiny/$test_name.out"
    local disasm_expect="$SCRIPT_DIR/tiny/$test_name.disasm"
    local bytecode_file="$SCRIPT_DIR/tiny/$test_name.pbc"

    local details=()
    local status="PASS"

    local actual_out=$(mktemp)
    local actual_disasm=$(mktemp)

    if ! "$TINY_BIN" "$src" "$bytecode_file"; then
        status="FAIL"
        details+=("tiny assembler failed")
    fi

    if [ "$status" = "PASS" ]; then
        if [ -f "$input_file" ]; then
            "$VM_BIN" "$bytecode_file" < "$input_file" > "$actual_out"
        else
            "$VM_BIN" "$bytecode_file" > "$actual_out"
        fi
        local run_status=$?

        strip_ansi_inplace "$actual_out"

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
        else
            if [ -s "$actual_out" ]; then
                status="FAIL"
                details+=("Unexpected stdout:\n$(cat "$actual_out")")
            fi
        fi

        if [ $run_status -ne 0 ]; then
            status="FAIL"
            details+=("pscalvm exited with $run_status")
        fi
    fi

    if [ "$status" = "PASS" ] && [ "$HAS_DCOMP" -eq 1 ] && [ -f "$disasm_expect" ]; then
        "$DCOMP_BIN" "$bytecode_file" 2> "$actual_disasm"
        sed -i.bak "s|$bytecode_file|tiny/$test_name.pbc|" "$actual_disasm" 2>/dev/null || true
        rm -f "$actual_disasm.bak"
        strip_ansi_inplace "$actual_disasm"
        set +e
        diff_output=$(diff -u "$disasm_expect" "$actual_disasm")
        local diff_status=$?
        set -e
        if [ $diff_status -ne 0 ]; then
            status="FAIL"
            if [ $diff_status -eq 1 ]; then
                details+=("Disassembly mismatch:\n$diff_output")
            else
                details+=("Disassembly diff failed with status $diff_status")
            fi
        fi
    elif [ "$HAS_DCOMP" -eq 0 ] && [ -f "$disasm_expect" ]; then
        details+=("Disassembly check skipped: pscald unavailable")
    fi

    rm -f "$bytecode_file" "$actual_out" "$actual_disasm"

    if [ "$status" = "PASS" ]; then
        harness_report PASS "tiny_${test_name}" "Tiny fixture $test_name"
    else
        harness_report FAIL "tiny_${test_name}" "Tiny fixture $test_name" "${details[@]}"
    fi
}

if [ ! -x "$TINY_BIN" ]; then
    echo "tiny script not found at $TINY_BIN" >&2
    exit 1
fi

if [ ! -x "$VM_BIN" ]; then
    echo "pscalvm binary not found at $VM_BIN" >&2
    exit 1
fi

HAS_DCOMP=1
if [ ! -x "$DCOMP_BIN" ]; then
    HAS_DCOMP=0
    harness_report SKIP "tiny_disassembly" "pscald disassembly checks" "pscald not found at $DCOMP_BIN"
fi

shopt -s nullglob
for src in "$SCRIPT_DIR"/tiny/*.tiny; do
    test_name=$(basename "$src" .tiny)
    run_tiny_fixture "$src" "$test_name"
done
shopt -u nullglob

harness_summary "Tiny"
if harness_exit_code; then
    exit 0
fi
exit 1
