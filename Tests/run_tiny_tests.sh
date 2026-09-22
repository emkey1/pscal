#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
TINY_BIN="$ROOT_DIR/tools/tiny"
VM_BIN="$ROOT_DIR/build/bin/pscalvm"
DCOMP_BIN="$ROOT_DIR/build/bin/pscald"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"
TINY_CLIKE_SRC="$ROOT_DIR/bin/tiny.clike"
TINY_PBC="$ROOT_DIR/build/bin/tiny.pbc"

. "$SCRIPT_DIR/tools/harness_utils.sh"
harness_init

strip_ansi_inplace() {
    local path="$1"
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

# Runs a compiled fixture under pscalvm (stdin from its .in, if any) and checks
# the exit status and stdout against its .out. Records problems in the
# caller's `status` and `details`.
run_compiled_fixture() {
    local bytecode_file="$1"
    local test_name="$2"
    local input_file="$SCRIPT_DIR/tiny/$test_name.in"
    local stdout_expect="$SCRIPT_DIR/tiny/$test_name.out"
    local actual_out actual_err
    actual_out=$(mktemp)
    actual_err=$(mktemp)

    local run_status=0
    if [ -f "$input_file" ]; then
        "$VM_BIN" "$bytecode_file" < "$input_file" > "$actual_out" 2> "$actual_err" || run_status=$?
    else
        "$VM_BIN" "$bytecode_file" < /dev/null > "$actual_out" 2> "$actual_err" || run_status=$?
    fi

    strip_ansi_inplace "$actual_out"

    if [ -f "$stdout_expect" ]; then
        set +e
        diff_output=$(diff -u "$stdout_expect" "$actual_out")
        local diff_status=$?
        set -e
        if [ $diff_status -ne 0 ]; then
            status="FAIL"
            if [ $diff_status -eq 1 ]; then
                details+=("stdout mismatch:" "$diff_output")
            else
                details+=("diff on stdout failed with status $diff_status")
            fi
        fi
    elif [ -s "$actual_out" ]; then
        status="FAIL"
        details+=("Unexpected stdout:" "$(cat "$actual_out")")
    fi

    if [ $run_status -ne 0 ]; then
        status="FAIL"
        details+=("pscalvm exited with $run_status:" "$(cat "$actual_err")")
    fi

    rm -f "$actual_out" "$actual_err"
}

# The reference compiler: tools/tiny (Python). Leaves its output at
# $SCRIPT_DIR/tiny/$test_name.pbc for the comparisons below.
run_tiny_fixture() {
    local src="$1"
    local test_name="$2"
    local disasm_expect="$SCRIPT_DIR/tiny/$test_name.disasm"
    local bytecode_file="$SCRIPT_DIR/tiny/$test_name.pbc"

    local details=()
    local status="PASS"

    local actual_disasm
    actual_disasm=$(mktemp)

    if ! "$TINY_BIN" "$src" "$bytecode_file"; then
        status="FAIL"
        details+=("tiny assembler failed")
    fi

    if [ "$status" = "PASS" ]; then
        run_compiled_fixture "$bytecode_file" "$test_name"
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
                details+=("Disassembly mismatch:" "$diff_output")
            else
                details+=("Disassembly diff failed with status $diff_status")
            fi
        fi
    elif [ "$HAS_DCOMP" -eq 0 ] && [ -f "$disasm_expect" ]; then
        details+=("Disassembly check skipped: pscald unavailable")
    fi

    rm -f "$actual_disasm"

    if [ "$status" = "PASS" ]; then
        harness_report PASS "tiny_${test_name}" "Tiny fixture $test_name"
    else
        harness_report FAIL "tiny_${test_name}" "Tiny fixture $test_name" "${details[@]}"
    fi
}

# bin/tiny.clike, the CLike port of the same compiler, run either as source
# under clike or precompiled (build/bin/tiny.pbc) under pscalvm. Its output
# must load and run, and must match tools/tiny's byte for byte.
run_tiny_clike_fixture() {
    local mode="$1"
    local src="$2"
    local test_name="$3"
    local reference_file="$SCRIPT_DIR/tiny/$test_name.pbc"
    local bytecode_file="$SCRIPT_DIR/tiny/$test_name.$mode.pbc"
    local compile_log
    compile_log=$(mktemp)

    local details=()
    local status="PASS"

    local compile_status=0
    if [ "$mode" = "pbc" ]; then
        PSCALI_WORKSPACE_ROOT="$ROOT_DIR" "$VM_BIN" "$TINY_PBC" "$src" "$bytecode_file" \
            > "$compile_log" 2>&1 || compile_status=$?
    else
        "$CLIKE_BIN" --no-cache "$TINY_CLIKE_SRC" "$src" "$bytecode_file" \
            > "$compile_log" 2>&1 || compile_status=$?
    fi

    if [ $compile_status -ne 0 ] || [ ! -f "$bytecode_file" ]; then
        status="FAIL"
        details+=("compile failed (status $compile_status):" "$(cat "$compile_log")")
    else
        if [ -f "$reference_file" ] && ! cmp -s "$reference_file" "$bytecode_file"; then
            status="FAIL"
            details+=("output differs from tools/tiny's $reference_file")
        fi
        run_compiled_fixture "$bytecode_file" "$test_name"
    fi

    rm -f "$bytecode_file" "$compile_log"

    local label="tiny.clike under clike"
    [ "$mode" = "pbc" ] && label="tiny.pbc under pscalvm"
    if [ "$status" = "PASS" ]; then
        harness_report PASS "tiny_${mode}_${test_name}" "Tiny fixture $test_name ($label)"
    else
        harness_report FAIL "tiny_${mode}_${test_name}" "Tiny fixture $test_name ($label)" "${details[@]}"
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

CLIKE_MODES=()
if [ -f "$TINY_PBC" ]; then
    CLIKE_MODES+=(pbc)
else
    harness_report SKIP "tiny_pbc" "tiny.pbc under pscalvm" "tiny.pbc not found at $TINY_PBC (build the tiny_pbc target)"
fi
if [ -x "$CLIKE_BIN" ]; then
    CLIKE_MODES+=(clike)
else
    harness_report SKIP "tiny_clike" "tiny.clike under clike" "clike not found at $CLIKE_BIN"
fi

shopt -s nullglob
for src in "$SCRIPT_DIR"/tiny/*.tiny; do
    test_name=$(basename "$src" .tiny)
    run_tiny_fixture "$src" "$test_name"
    for mode in ${CLIKE_MODES[@]+"${CLIKE_MODES[@]}"}; do
        run_tiny_clike_fixture "$mode" "$src" "$test_name"
    done
    rm -f "$SCRIPT_DIR/tiny/$test_name.pbc"
done
shopt -u nullglob

harness_summary "Tiny"
if harness_exit_code; then
    exit 0
fi
exit 1
