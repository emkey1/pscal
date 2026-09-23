#!/usr/bin/env bash
# pscalvm must run a frontend's bytecode the way that frontend runs it.
#
# pscalvm is the only host with no frontend of its own: it is handed a .bc and
# nothing else. It used to assume Pascal, which is wrong for any chunk a
# 0-based-string frontend compiled -- an Aether program that indexed or
# iterated a Text died with "String index 0 out of bounds", and copy()/pos()
# came back off by one with no error at all. The PSB3 container now records
# which FrontendKind compiled the chunk (core/cache.c, header flags bits 0..7)
# and pscalvm adopts it.
#
# Each case below runs a fixture under its own frontend, then runs the .bc that
# run cached under pscalvm, and requires the two to agree byte for byte on
# stdout, stderr and exit status. That is the property that actually matters,
# and it is checked in both directions: Aether (0-based) must stay 0-based, and
# rea/clike (1-based) must not get swept along with it.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
FIXTURE_DIR="$SCRIPT_DIR/pscalvm_frontend"
VM_BIN="$ROOT_DIR/build/bin/pscalvm"
PSCALD_BIN="$ROOT_DIR/build/bin/pscald"
PSCALASM_BIN="$ROOT_DIR/build/bin/pscalasm"

. "$SCRIPT_DIR/tools/harness_utils.sh"
harness_init

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# FrontendKind wire codes, common/frontend_kind.h. Spelled out here rather than
# derived so that renumbering the enum -- which would silently reinterpret
# every .bc ever written -- breaks this test.
frontend_code() {
    case "$1" in
        pascal) echo 1 ;;
        rea)    echo 2 ;;
        aether) echo 3 ;;
        clike)  echo 4 ;;
        shell)  echo 5 ;;
        *)      echo "" ;;
    esac
}

# The frontend field is byte 8 of the file: magic u32, format_ver u16,
# vm_ver u16, then the flags word whose low byte this is (little-endian).
read_frontend_byte() {
    od -An -tu1 -j8 -N1 "$1" | tr -d ' \n'
}

# Runs $fixture under $frontend_bin with a private HOME so the bytecode cache
# it writes is ours alone, then runs that cached .bc under pscalvm.
run_case() {
    local test_id="$1" frontend="$2" frontend_bin="$3" fixture="$4"
    local details=() status="PASS"

    if [ ! -x "$frontend_bin" ]; then
        harness_report SKIP "$test_id" "$frontend vs pscalvm on $(basename "$fixture")" \
            "missing $frontend_bin"
        return
    fi

    local case_home="$WORK_DIR/$test_id"
    mkdir -p "$case_home"

    local front_out="$WORK_DIR/$test_id.front.out" front_err="$WORK_DIR/$test_id.front.err"
    local vm_out="$WORK_DIR/$test_id.vm.out" vm_err="$WORK_DIR/$test_id.vm.err"
    local front_rc=0 vm_rc=0

    HOME="$case_home" "$frontend_bin" "$fixture" >"$front_out" 2>"$front_err" || front_rc=$?

    # One fixture per private HOME, so the cache holds exactly one entry.
    local bc_files=("$case_home"/.pscal/bc_cache/*.bc)
    if [ ! -f "${bc_files[0]}" ]; then
        harness_report FAIL "$test_id" "$frontend vs pscalvm on $(basename "$fixture")" \
            "the $frontend run left no .bc in $case_home/.pscal/bc_cache" \
            "$(cat "$front_out" "$front_err")"
        return
    fi
    local bc="${bc_files[0]}"

    local want_code actual_code
    want_code="$(frontend_code "$frontend")"
    actual_code="$(read_frontend_byte "$bc")"
    if [ "$actual_code" != "$want_code" ]; then
        status="FAIL"
        details+=("PSB3 header frontend field is $actual_code, expected $want_code ($frontend)")
    fi

    "$VM_BIN" "$bc" >"$vm_out" 2>"$vm_err" || vm_rc=$?

    if [ "$front_rc" -ne "$vm_rc" ]; then
        status="FAIL"
        details+=("exit status differs: $frontend $front_rc, pscalvm $vm_rc")
    fi
    local stream diff_output
    for stream in out err; do
        diff_output="$(diff -u "$WORK_DIR/$test_id.front.$stream" "$WORK_DIR/$test_id.vm.$stream")"
        if [ -n "$diff_output" ]; then
            status="FAIL"
            details+=("std$stream differs:" "$diff_output")
        fi
    done

    if [ "$status" = "PASS" ]; then
        harness_report PASS "$test_id" "$frontend vs pscalvm on $(basename "$fixture")"
    else
        harness_report FAIL "$test_id" "$frontend vs pscalvm on $(basename "$fixture")" "${details[@]}"
    fi
}

# `pscald --emit-asm | pscalasm` is the one path that rebuilds a chunk from
# text rather than copying its bytes, so it is the one path that can drop the
# frontend field. The .asm carries it as a `frontend` directive.
#
# Hand-written rather than dumped from a real Aether chunk: `pscald --emit-asm`
# cannot emit a UNICODESTRING constant yet ("unsupported constant type in
# --emit-asm"), which every Aether Text local produces, so no frontend's own
# output can round-trip through it today. That gap is about which constant
# types emitAsmV2() covers and is unrelated to the frontend field, so this case
# tests the field on bytecode emitAsmV2() can already handle.
#
# The five instructions index a string at 0, which is the whole point: the same
# code bytes run clean under `frontend aether` and fail with "String index (0)
# out of bounds" under `frontend pascal`. If the directive stops being written,
# read or preserved, this case stops passing rather than quietly testing
# nothing.
run_asm_roundtrip_case() {
    local test_id="asm_roundtrip_frontend"
    local description="pscald/pscalasm round-trip keeps the frontend"
    local details=() status="PASS"

    if [ ! -x "$PSCALD_BIN" ] || [ ! -x "$PSCALASM_BIN" ]; then
        harness_report SKIP "$test_id" "$description" "missing $PSCALD_BIN or $PSCALASM_BIN"
        return
    fi

    local asm="$WORK_DIR/$test_id.asm"
    cat > "$asm" <<'ASM'
PSCALASM2
version 9
frontend aether
constants 1
const 0 4 "abc"
builtin_map 0
const_symbols 0
procedures 0
code 6
inst 1 CONSTANT 0
inst 1 CONST_0
inst 1 GET_CHAR_FROM_STRING
inst 1 POP
inst 1 HALT
end
ASM

    local bc="$WORK_DIR/$test_id.bc"
    if ! "$PSCALASM_BIN" "$asm" "$bc" >"$WORK_DIR/$test_id.asm.err" 2>&1; then
        harness_report FAIL "$test_id" "$description" \
            "pscalasm rejected the frontend directive" "$(cat "$WORK_DIR/$test_id.asm.err")"
        return
    fi
    if [ "$(read_frontend_byte "$bc")" != "3" ]; then
        status="FAIL"
        details+=("assembled chunk did not record the aether frontend")
    fi

    local rc=0
    "$VM_BIN" "$bc" >"$WORK_DIR/$test_id.out" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        status="FAIL"
        details+=("s[0] failed under the aether frontend:" "$(cat "$WORK_DIR/$test_id.out")")
    fi

    # The control: the identical code bytes under Pascal's 1-based strings.
    # Without it, a bug that ignored the field entirely would still pass above.
    local pascal_asm="$WORK_DIR/$test_id.pascal.asm" pascal_bc="$WORK_DIR/$test_id.pascal.bc"
    sed 's/^frontend aether$/frontend pascal/' "$asm" >"$pascal_asm"
    if "$PSCALASM_BIN" "$pascal_asm" "$pascal_bc" >/dev/null 2>&1; then
        local pascal_rc=0
        "$VM_BIN" "$pascal_bc" >"$WORK_DIR/$test_id.pascal.out" 2>&1 || pascal_rc=$?
        if [ "$pascal_rc" -eq 0 ]; then
            status="FAIL"
            details+=("s[0] should be out of bounds under the pascal frontend, but it ran clean")
        fi
    else
        status="FAIL"
        details+=("pscalasm rejected the pascal control")
    fi

    # And back out through the disassembler, so the emit side is covered too.
    local reasm="$WORK_DIR/$test_id.emit.asm"
    if "$PSCALD_BIN" --emit-asm "$bc" >"$reasm" 2>/dev/null; then
        if ! grep -qx 'frontend aether' "$reasm"; then
            status="FAIL"
            details+=("pscald --emit-asm did not emit 'frontend aether'")
        fi
    else
        status="FAIL"
        details+=("pscald --emit-asm failed on the assembled chunk")
    fi

    if [ "$status" = "PASS" ]; then
        harness_report PASS "$test_id" "$description"
    else
        harness_report FAIL "$test_id" "$description" "${details[@]}"
    fi
}

# A chunk that names no frontend (a hand-written .asm, tools/tiny) must keep
# running under the Pascal-compatible defaults it always had.
run_unknown_frontend_case() {
    local test_id="unknown_frontend_defaults"
    local description="a chunk with no recorded frontend still runs as before"

    if [ ! -x "$PSCALASM_BIN" ]; then
        harness_report SKIP "$test_id" "$description" "missing $PSCALASM_BIN"
        return
    fi

    local asm="$WORK_DIR/$test_id.asm" bc="$WORK_DIR/$test_id.bc"
    cat > "$asm" <<'ASM'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 0
procedures 0
code 1
inst 0 HALT
end
ASM
    if ! "$PSCALASM_BIN" "$asm" "$bc" >"$WORK_DIR/$test_id.err" 2>&1; then
        harness_report FAIL "$test_id" "$description" \
            "pscalasm failed" "$(cat "$WORK_DIR/$test_id.err")"
        return
    fi
    local code
    code="$(read_frontend_byte "$bc")"
    if [ "$code" != "0" ]; then
        harness_report FAIL "$test_id" "$description" \
            "expected frontend field 0 (unknown), got $code"
        return
    fi
    local rc=0
    "$VM_BIN" "$bc" >"$WORK_DIR/$test_id.out" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        harness_report FAIL "$test_id" "$description" \
            "pscalvm exited $rc" "$(cat "$WORK_DIR/$test_id.out")"
        return
    fi
    harness_report PASS "$test_id" "$description"
}

if [ ! -x "$VM_BIN" ]; then
    echo "pscalvm binary not found at $VM_BIN" >&2
    exit 1
fi

run_case aether_string_ops   aether "$ROOT_DIR/build/bin/aether" "$FIXTURE_DIR/aether_string_ops.aether"
run_case aether_array_bounds aether "$ROOT_DIR/build/bin/aether" "$FIXTURE_DIR/aether_array_bounds.aether"
run_case clike_string_ops    clike  "$ROOT_DIR/build/bin/clike"  "$FIXTURE_DIR/clike_string_ops.clike"
run_case rea_string_ops      rea    "$ROOT_DIR/build/bin/rea"    "$FIXTURE_DIR/rea_string_ops.rea"
run_asm_roundtrip_case
run_unknown_frontend_case

harness_summary "pscalvm frontend"
if harness_exit_code; then
    exit 0
fi
exit 1
