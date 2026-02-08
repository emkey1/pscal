#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PSCALASM_BIN="$ROOT_DIR/build/bin/pscalasm"
PSCALD_BIN="$ROOT_DIR/build/bin/pscald"
VM_BIN="$ROOT_DIR/build/bin/pscalvm"
PASCAL_BIN="$ROOT_DIR/build/bin/pascal"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"
REA_BIN="$ROOT_DIR/build/bin/rea"

. "$SCRIPT_DIR/tools/harness_utils.sh"
harness_init

fail_with_details() {
    local test_id="$1"
    local description="$2"
    shift 2
    harness_report FAIL "$test_id" "$description" "$@"
}

require_tools() {
    local missing=0
    if [ ! -x "$PSCALASM_BIN" ]; then
        harness_report SKIP "pscalasm_missing" "pscalasm harness prerequisites" "missing $PSCALASM_BIN"
        missing=1
    fi
    if [ ! -x "$PSCALD_BIN" ]; then
        harness_report SKIP "pscald_missing" "pscalasm harness prerequisites" "missing $PSCALD_BIN"
        missing=1
    fi
    if [ ! -x "$VM_BIN" ]; then
        harness_report SKIP "pscalvm_missing" "pscalasm harness prerequisites" "missing $VM_BIN"
        missing=1
    fi
    return $missing
}

run_minimal_pscalasm2_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/min.asm" <<'EOF'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 0
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/min.asm" "$tmpd/min.pbc"; then
        fail_with_details "pscalasm_minimal" "assemble minimal program" "pscalasm failed to assemble minimal PSCALASM2 source"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/min.pbc" > "$tmpd/min.out"; then
        fail_with_details "pscalasm_minimal" "assemble minimal program" "pscalvm execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if [ -s "$tmpd/min.out" ]; then
        fail_with_details "pscalasm_minimal" "assemble minimal program" "expected empty stdout, got:\n$(cat "$tmpd/min.out")"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/min.pbc" > "$tmpd/min.emit.asm" 2> "$tmpd/min.disasm"; then
        fail_with_details "pscalasm_minimal" "assemble minimal program" "pscald --emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q '^inst 0 HALT$' "$tmpd/min.emit.asm"; then
        fail_with_details "pscalasm_minimal" "assemble minimal program" "expected emitted HALT instruction not found"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_minimal" "assemble minimal program"
    trap - RETURN
    rm -rf "$tmpd"
}

run_labels_and_proc_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/proc.asm" <<'EOF'
PSCALASM2
version 9
constants 1
const 0 4 "foo"
builtin_map 0
const_symbols 0
procedures 1
proc 0 "foo" 3 0 0 1 0 -1
code 10
inst 1 JUMP @main
label L0003
inst 2 CONST_TRUE
inst 2 RETURN
label main
inst 3 CALL_USER_PROC 0 0 0
inst 3 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/proc.asm" "$tmpd/proc.pbc"; then
        fail_with_details "pscalasm_labels_proc" "label+procedure round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$PSCALD_BIN" --emit-asm "$tmpd/proc.pbc" > "$tmpd/proc.emit.asm" 2> "$tmpd/proc.disasm"; then
        fail_with_details "pscalasm_labels_proc" "label+procedure round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$PSCALASM_BIN" "$tmpd/proc.emit.asm" "$tmpd/proc.rebuilt.pbc"; then
        fail_with_details "pscalasm_labels_proc" "label+procedure round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/proc.pbc" > "$tmpd/proc.out"; then
        fail_with_details "pscalasm_labels_proc" "label+procedure round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/proc.rebuilt.pbc" > "$tmpd/proc.rebuilt.out"; then
        fail_with_details "pscalasm_labels_proc" "label+procedure round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/proc.out" "$tmpd/proc.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_labels_proc" "label+procedure round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q '^label L[0-9]{4}$' "$tmpd/proc.emit.asm"; then
        fail_with_details "pscalasm_labels_proc" "label+procedure round-trip" "no symbolic labels emitted"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_labels_proc" "label+procedure round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_extended_constants_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/constants.asm" <<'EOF'
PSCALASM2
version 9
constants 4
const 0 10 "Color" 2
const 1 14 3 1 3 5
const 2 11 dims 1 elem 2 bounds 0 2 values 3 10 20 30
const 3 15 null
builtin_map 0
const_symbols 1
const_symbol "answer" 2 42
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/constants.asm" "$tmpd/constants.pbc"; then
        fail_with_details "pscalasm_constants" "extended constants and const symbols" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/constants.pbc" > "$tmpd/constants.emit.asm" 2> "$tmpd/constants.disasm"; then
        fail_with_details "pscalasm_constants" "extended constants and const symbols" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q 'const_symbol "answer" 2 42' "$tmpd/constants.emit.asm"; then
        fail_with_details "pscalasm_constants" "extended constants and const symbols" "const_symbol metadata missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/constants.emit.asm" "$tmpd/constants.rebuilt.pbc"; then
        fail_with_details "pscalasm_constants" "extended constants and const symbols" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/constants.pbc" > "$tmpd/constants.out"; then
        fail_with_details "pscalasm_constants" "extended constants and const symbols" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/constants.rebuilt.pbc" > "$tmpd/constants.rebuilt.out"; then
        fail_with_details "pscalasm_constants" "extended constants and const symbols" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/constants.out" "$tmpd/constants.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_constants" "extended constants and const symbols" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_constants" "extended constants and const symbols"
    trap - RETURN
    rm -rf "$tmpd"
}

run_types_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/types.asm" <<'EOF'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 0
types 1
type "AliasInt" "{\"node_type\":\"INTERFACE\",\"var_type_annotated\":\"INTEGER\"}"
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/types.asm" "$tmpd/types.pbc"; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/types.pbc" > "$tmpd/types.emit.asm" 2> "$tmpd/types.disasm"; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q '^types 1$' "$tmpd/types.emit.asm"; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "types section missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! rg -q '^type "AliasInt" "' "$tmpd/types.emit.asm"; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "type entry missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/types.emit.asm" "$tmpd/types.rebuilt.pbc"; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/types.pbc" > "$tmpd/types.out"; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/types.rebuilt.pbc" > "$tmpd/types.rebuilt.out"; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/types.out" "$tmpd/types.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_types" "types metadata round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_types" "types metadata round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_shellfn_pointer_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/pointer.asm" <<'EOF'
PSCALASM2
version 9
constants 1
const 0 15 shellfn_asm "PSCALASM2\nversion 9\nconstants 0\nbuiltin_map 0\nconst_symbols 0\ntypes 0\nprocedures 0\ncode 1\ninst 0 HALT\nend\n"
builtin_map 0
const_symbols 0
types 0
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/pointer.asm" "$tmpd/pointer.pbc"; then
        fail_with_details "pscalasm_shellfn_pointer" "shell function pointer constant round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/pointer.pbc" > "$tmpd/pointer.emit.asm" 2> "$tmpd/pointer.disasm"; then
        fail_with_details "pscalasm_shellfn_pointer" "shell function pointer constant round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q 'shellfn_asm "' "$tmpd/pointer.emit.asm"; then
        fail_with_details "pscalasm_shellfn_pointer" "shell function pointer constant round-trip" "shellfn_asm payload missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/pointer.emit.asm" "$tmpd/pointer.rebuilt.pbc"; then
        fail_with_details "pscalasm_shellfn_pointer" "shell function pointer constant round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/pointer.pbc" > "$tmpd/pointer.out"; then
        fail_with_details "pscalasm_shellfn_pointer" "shell function pointer constant round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/pointer.rebuilt.pbc" > "$tmpd/pointer.rebuilt.out"; then
        fail_with_details "pscalasm_shellfn_pointer" "shell function pointer constant round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/pointer.out" "$tmpd/pointer.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_shellfn_pointer" "shell function pointer constant round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_shellfn_pointer" "shell function pointer constant round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_charptr_pointer_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/charptr.asm" <<'EOF'
PSCALASM2
version 9
constants 1
const 0 15 charptr "hello-pointer"
builtin_map 0
const_symbols 0
types 0
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/charptr.asm" "$tmpd/charptr.pbc"; then
        fail_with_details "pscalasm_charptr_pointer" "char pointer constant round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/charptr.pbc" > "$tmpd/charptr.emit.asm" 2> "$tmpd/charptr.disasm"; then
        fail_with_details "pscalasm_charptr_pointer" "char pointer constant round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q 'charptr "hello-pointer"' "$tmpd/charptr.emit.asm"; then
        fail_with_details "pscalasm_charptr_pointer" "char pointer constant round-trip" "charptr payload missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/charptr.emit.asm" "$tmpd/charptr.rebuilt.pbc"; then
        fail_with_details "pscalasm_charptr_pointer" "char pointer constant round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/charptr.pbc" > "$tmpd/charptr.out"; then
        fail_with_details "pscalasm_charptr_pointer" "char pointer constant round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/charptr.rebuilt.pbc" > "$tmpd/charptr.rebuilt.out"; then
        fail_with_details "pscalasm_charptr_pointer" "char pointer constant round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/charptr.out" "$tmpd/charptr.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_charptr_pointer" "char pointer constant round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_charptr_pointer" "char pointer constant round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_opaque_pointer_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/opaque.asm" <<'EOF'
PSCALASM2
version 9
constants 1
const 0 15 opaque_addr 0x1234
builtin_map 0
const_symbols 0
types 0
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/opaque.asm" "$tmpd/opaque.pbc"; then
        fail_with_details "pscalasm_opaque_pointer" "opaque pointer constant round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/opaque.pbc" > "$tmpd/opaque.emit.asm" 2> "$tmpd/opaque.disasm"; then
        fail_with_details "pscalasm_opaque_pointer" "opaque pointer constant round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q 'opaque_addr' "$tmpd/opaque.emit.asm"; then
        fail_with_details "pscalasm_opaque_pointer" "opaque pointer constant round-trip" "opaque pointer payload missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/opaque.emit.asm" "$tmpd/opaque.rebuilt.pbc"; then
        fail_with_details "pscalasm_opaque_pointer" "opaque pointer constant round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/opaque.pbc" > "$tmpd/opaque.out"; then
        fail_with_details "pscalasm_opaque_pointer" "opaque pointer constant round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/opaque.rebuilt.pbc" > "$tmpd/opaque.rebuilt.out"; then
        fail_with_details "pscalasm_opaque_pointer" "opaque pointer constant round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/opaque.out" "$tmpd/opaque.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_opaque_pointer" "opaque pointer constant round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_opaque_pointer" "opaque pointer constant round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_pointer_const_symbol_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/pointer_const_symbol.asm" <<'EOF'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 2
const_symbol "char_ptr_symbol" 15 charptr "hello-const-symbol"
const_symbol "opaque_ptr_symbol" 15 opaque_addr 0x1234
types 0
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/pointer_const_symbol.asm" "$tmpd/pointer_const_symbol.pbc"; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/pointer_const_symbol.pbc" > "$tmpd/pointer_const_symbol.emit.asm" 2> "$tmpd/pointer_const_symbol.disasm"; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q 'const_symbol "char_ptr_symbol" 15 charptr "hello-const-symbol"' "$tmpd/pointer_const_symbol.emit.asm"; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "charptr const_symbol missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q 'const_symbol "opaque_ptr_symbol" 15 opaque_addr ' "$tmpd/pointer_const_symbol.emit.asm"; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "opaque const_symbol missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/pointer_const_symbol.emit.asm" "$tmpd/pointer_const_symbol.rebuilt.pbc"; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/pointer_const_symbol.pbc" > "$tmpd/pointer_const_symbol.out"; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/pointer_const_symbol.rebuilt.pbc" > "$tmpd/pointer_const_symbol.rebuilt.out"; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/pointer_const_symbol.out" "$tmpd/pointer_const_symbol.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_pointer_const_symbol" "pointer const_symbol round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_builtin_map_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/builtin_map.asm" <<'EOF'
PSCALASM2
version 9
constants 2
const 0 4 "Write"
const 1 4 "write"
builtin_map 1
builtin 0 1
const_symbols 0
types 0
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/builtin_map.asm" "$tmpd/builtin_map.pbc"; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/builtin_map.pbc" > "$tmpd/builtin_map.emit.asm" 2> "$tmpd/builtin_map.disasm"; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q '^builtin_map 1$' "$tmpd/builtin_map.emit.asm"; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "builtin_map header missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! rg -q '^builtin 0 1$' "$tmpd/builtin_map.emit.asm"; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "builtin entry missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/builtin_map.emit.asm" "$tmpd/builtin_map.rebuilt.pbc"; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/builtin_map.pbc" > "$tmpd/builtin_map.out"; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/builtin_map.rebuilt.pbc" > "$tmpd/builtin_map.rebuilt.out"; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/builtin_map.out" "$tmpd/builtin_map.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_builtin_map" "builtin map round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_builtin_map" "builtin map round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_procedure_upvalue_roundtrip_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/proc_upvalue.asm" <<'EOF'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 0
types 0
procedures 2
proc 0 "outer" 0 0 0 0 0 -1
proc 1 "inner" 0 0 1 0 0 0
upvalue 1 0 0 1 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/proc_upvalue.asm" "$tmpd/proc_upvalue.pbc"; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "initial assembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$tmpd/proc_upvalue.pbc" > "$tmpd/proc_upvalue.emit.asm" 2> "$tmpd/proc_upvalue.disasm"; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "emit-asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! rg -q '^procedures 2$' "$tmpd/proc_upvalue.emit.asm"; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "procedures header missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! rg -q '^upvalue [0-9]+ 0 0 1 0$' "$tmpd/proc_upvalue.emit.asm"; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "upvalue metadata missing from emit output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/proc_upvalue.emit.asm" "$tmpd/proc_upvalue.rebuilt.pbc"; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/proc_upvalue.pbc" > "$tmpd/proc_upvalue.out"; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "original VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/proc_upvalue.rebuilt.pbc" > "$tmpd/proc_upvalue.rebuilt.out"; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/proc_upvalue.out" "$tmpd/proc_upvalue.rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_proc_upvalue" "procedure upvalue metadata round-trip"
    trap - RETURN
    rm -rf "$tmpd"
}

run_operand_validation_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/invalid_halt.asm" <<'EOF'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 0
procedures 0
code 2
inst 0 HALT 1
end
EOF

    if "$PSCALASM_BIN" "$tmpd/invalid_halt.asm" "$tmpd/invalid_halt.pbc" > "$tmpd/invalid_halt.out" 2> "$tmpd/invalid_halt.err"; then
        fail_with_details "pscalasm_operand_validation" "reject invalid operand shapes" "HALT with extra operand unexpectedly assembled"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    cat > "$tmpd/invalid_init_array.asm" <<'EOF'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 0
procedures 0
code 4
inst 0 INIT_LOCAL_ARRAY 0 0 0
end
EOF

    if "$PSCALASM_BIN" "$tmpd/invalid_init_array.asm" "$tmpd/invalid_init_array.pbc" > "$tmpd/invalid_init_array.out" 2> "$tmpd/invalid_init_array.err"; then
        fail_with_details "pscalasm_operand_validation" "reject invalid operand shapes" "INIT_LOCAL_ARRAY with too few operands unexpectedly assembled"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_operand_validation" "reject invalid operand shapes"
    trap - RETURN
    rm -rf "$tmpd"
}

run_legacy_block_compat_test() {
    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    cat > "$tmpd/base.asm" <<'EOF'
PSCALASM2
version 9
constants 0
builtin_map 0
const_symbols 0
procedures 0
code 1
inst 0 HALT
end
EOF

    if ! "$PSCALASM_BIN" "$tmpd/base.asm" "$tmpd/base.pbc"; then
        fail_with_details "pscalasm_legacy" "legacy --asm block compatibility" "failed to build baseline pbc"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --asm "$tmpd/base.pbc" 2> "$tmpd/base.legacy.txt"; then
        fail_with_details "pscalasm_legacy" "legacy --asm block compatibility" "pscald --asm failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALASM_BIN" "$tmpd/base.legacy.txt" "$tmpd/base.legacy.rebuilt.pbc"; then
        fail_with_details "pscalasm_legacy" "legacy --asm block compatibility" "legacy reassembly failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$tmpd/base.pbc" > "$tmpd/base.out"; then
        fail_with_details "pscalasm_legacy" "legacy --asm block compatibility" "baseline VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi
    if ! "$VM_BIN" "$tmpd/base.legacy.rebuilt.pbc" > "$tmpd/base.legacy.out"; then
        fail_with_details "pscalasm_legacy" "legacy --asm block compatibility" "legacy rebuilt VM execution failed"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/base.out" "$tmpd/base.legacy.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "pscalasm_legacy" "legacy --asm block compatibility" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "pscalasm_legacy" "legacy --asm block compatibility"
    trap - RETURN
    rm -rf "$tmpd"
}

run_real_source_roundtrip_test() {
    local test_id="$1"
    local description="$2"
    local compiler_id="$3"
    local compiler_bin="$4"
    local fixture_rel="$5"
    local source_ext="$6"
    local source_mode="${7:-copy}"

    if [ ! -x "$compiler_bin" ]; then
        harness_report SKIP "$test_id" "$description" "missing compiler binary $compiler_bin"
        return
    fi

    local fixture="$ROOT_DIR/$fixture_rel"
    if [ ! -f "$fixture" ]; then
        fail_with_details "$test_id" "$description" "missing fixture $fixture"
        return
    fi

    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    local test_home="$tmpd/home"
    local cache_dir="$test_home/.pscal/bc_cache"
    mkdir -p "$test_home"

    local source_name=""
    local source_path=""
    if [ "$source_mode" = "in_place" ]; then
        source_name="$(basename "$fixture")"
        source_path="$fixture"
    else
        source_name="pscalasm_rt_${compiler_id}_$$.${source_ext}"
        source_path="$tmpd/$source_name"
        cp "$fixture" "$source_path"
    fi

    if ! HOME="$test_home" "$compiler_bin" --dump-bytecode-only "$source_path" > "$tmpd/${compiler_id}.compile.out" 2> "$tmpd/${compiler_id}.compile.err"; then
        fail_with_details "$test_id" "$description" "source compilation failed:\n$(sed -n '1,160p' "$tmpd/${compiler_id}.compile.err")"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if [ ! -d "$cache_dir" ]; then
        fail_with_details "$test_id" "$description" "cache directory missing after compile: $cache_dir"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    local original_pbc
    original_pbc="$(ls -1t "$cache_dir"/"${compiler_id}-${source_name}-"*.bc 2>/dev/null | head -n 1 || true)"
    if [ -z "$original_pbc" ] || [ ! -f "$original_pbc" ]; then
        fail_with_details "$test_id" "$description" "compiled cache bytecode not found for source $source_name"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" --emit-asm "$original_pbc" > "$tmpd/original.emit.asm" 2> "$tmpd/original.disasm"; then
        fail_with_details "$test_id" "$description" "pscald --emit-asm failed for compiled bytecode"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    local rebuilt_pbc="$tmpd/rebuilt.pbc"
    if ! "$PSCALASM_BIN" "$tmpd/original.emit.asm" "$rebuilt_pbc"; then
        fail_with_details "$test_id" "$description" "pscalasm failed to rebuild emitted assembly"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$original_pbc" > "$tmpd/original.out"; then
        fail_with_details "$test_id" "$description" "pscalvm failed on original compiled bytecode"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$VM_BIN" "$rebuilt_pbc" > "$tmpd/rebuilt.out"; then
        fail_with_details "$test_id" "$description" "pscalvm failed on rebuilt bytecode"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/original.out" "$tmpd/rebuilt.out")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "$test_id" "$description" "VM output mismatch:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" "$original_pbc" 2> "$tmpd/original.full.disasm"; then
        fail_with_details "$test_id" "$description" "pscald failed on original compiled bytecode"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if ! "$PSCALD_BIN" "$rebuilt_pbc" 2> "$tmpd/rebuilt.full.disasm"; then
        fail_with_details "$test_id" "$description" "pscald failed on rebuilt bytecode"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    perl -ne 'print unless /^== (Disassembly|End Disassembly): /' \
        "$tmpd/original.full.disasm" > "$tmpd/original.full.norm"
    perl -ne 'print unless /^== (Disassembly|End Disassembly): /' \
        "$tmpd/rebuilt.full.disasm" > "$tmpd/rebuilt.full.norm"

    set +e
    diff_output="$(diff -u "$tmpd/original.full.norm" "$tmpd/rebuilt.full.norm")"
    diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "$test_id" "$description" "disassembly mismatch after source-bytecode round-trip:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    harness_report PASS "$test_id" "$description"
    trap - RETURN
    rm -rf "$tmpd"
}

run_real_source_roundtrip_tests() {
    run_real_source_roundtrip_test \
        "pscalasm_source_roundtrip_pascal" \
        "source->bytecode cache round-trip (pascal)" \
        "pascal" \
        "$PASCAL_BIN" \
        "Tests/tools/fixtures/cli_pascal.pas" \
        "pas"

    run_real_source_roundtrip_test \
        "pscalasm_source_roundtrip_clike" \
        "source->bytecode cache round-trip (clike)" \
        "clike" \
        "$CLIKE_BIN" \
        "Tests/tools/fixtures/cli_clike.cl" \
        "cl"

    run_real_source_roundtrip_test \
        "pscalasm_source_roundtrip_rea" \
        "source->bytecode cache round-trip (rea)" \
        "rea" \
        "$REA_BIN" \
        "Tests/tools/fixtures/cli_rea.rea" \
        "rea"

    run_real_source_roundtrip_test \
        "pscalasm_source_roundtrip_heavy_pascal" \
        "source->bytecode cache round-trip (pascal nested routines)" \
        "pascal" \
        "$PASCAL_BIN" \
        "Tests/Pascal/NestedRoutineSuite" \
        "pas" \
        "in_place"

    run_real_source_roundtrip_test \
        "pscalasm_source_roundtrip_heavy_clike" \
        "source->bytecode cache round-trip (clike imports)" \
        "clike" \
        "$CLIKE_BIN" \
        "Tests/clike/ImportSimple.cl" \
        "cl" \
        "in_place"

    run_real_source_roundtrip_test \
        "pscalasm_source_roundtrip_heavy_rea" \
        "source->bytecode cache round-trip (rea constructors)" \
        "rea" \
        "$REA_BIN" \
        "Tests/rea/constructor_default.rea" \
        "rea" \
        "in_place"
}

run_negative_source_compile_test() {
    local test_id="$1"
    local description="$2"
    local compiler_id="$3"
    local compiler_bin="$4"
    local fixture_rel="$5"
    local source_ext="$6"

    if [ ! -x "$compiler_bin" ]; then
        harness_report SKIP "$test_id" "$description" "missing compiler binary $compiler_bin"
        return
    fi

    local fixture="$ROOT_DIR/$fixture_rel"
    if [ ! -f "$fixture" ]; then
        fail_with_details "$test_id" "$description" "missing fixture $fixture"
        return
    fi

    local tmpd
    tmpd="$(mktemp -d)"
    trap 'rm -rf "$tmpd"' RETURN

    local test_home="$tmpd/home"
    local cache_dir="$test_home/.pscal/bc_cache"
    mkdir -p "$test_home"

    local source_name="pscalasm_neg_${compiler_id}_$$.${source_ext}"
    local source_path="$tmpd/$source_name"
    cp "$fixture" "$source_path"

    set +e
    HOME="$test_home" "$compiler_bin" --dump-bytecode-only "$source_path" > "$tmpd/compile1.out" 2> "$tmpd/compile1.err"
    local rc1=$?
    HOME="$test_home" "$compiler_bin" --dump-bytecode-only "$source_path" > "$tmpd/compile2.out" 2> "$tmpd/compile2.err"
    local rc2=$?
    set -e

    if [ $rc1 -eq 0 ] || [ $rc2 -eq 0 ]; then
        fail_with_details "$test_id" "$description" \
            "invalid source unexpectedly compiled (rc1=$rc1 rc2=$rc2)\nrun1 stderr:\n$(sed -n '1,120p' "$tmpd/compile1.err")\nrun2 stderr:\n$(sed -n '1,120p' "$tmpd/compile2.err")"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    sed "s|$source_path|<SRC>|g" "$tmpd/compile1.err" > "$tmpd/compile1.norm.err"
    sed "s|$source_path|<SRC>|g" "$tmpd/compile2.err" > "$tmpd/compile2.norm.err"

    set +e
    local diff_output
    diff_output="$(diff -u "$tmpd/compile1.norm.err" "$tmpd/compile2.norm.err")"
    local diff_status=$?
    set -e
    if [ $diff_status -ne 0 ]; then
        fail_with_details "$test_id" "$description" \
            "nondeterministic compiler stderr across repeated failures:\n$diff_output"
        trap - RETURN
        rm -rf "$tmpd"
        return
    fi

    if [ -d "$cache_dir" ]; then
        local cache_match
        cache_match="$(ls -1 "$cache_dir"/"${compiler_id}-${source_name}-"*.bc 2>/dev/null | head -n 1 || true)"
        if [ -n "$cache_match" ]; then
            fail_with_details "$test_id" "$description" \
                "unexpected cache artifact for invalid source: $cache_match"
            trap - RETURN
            rm -rf "$tmpd"
            return
        fi
    fi

    harness_report PASS "$test_id" "$description"
    trap - RETURN
    rm -rf "$tmpd"
}

run_negative_source_compile_tests() {
    run_negative_source_compile_test \
        "pscalasm_negative_pascal" \
        "invalid source compile is stable (pascal)" \
        "pascal" \
        "$PASCAL_BIN" \
        "Tests/Pascal/ArgumentTypeMismatch" \
        "pas"

    run_negative_source_compile_test \
        "pscalasm_negative_clike" \
        "invalid source compile is stable (clike)" \
        "clike" \
        "$CLIKE_BIN" \
        "Tests/clike/TypeError.cl" \
        "cl"

    run_negative_source_compile_test \
        "pscalasm_negative_rea" \
        "invalid source compile is stable (rea)" \
        "rea" \
        "$REA_BIN" \
        "Tests/rea_negatives/continue_outside_loop.rea" \
        "rea"
}

run_pscalasm_unit_tests() {
    run_minimal_pscalasm2_test
    run_operand_validation_test
    run_legacy_block_compat_test
}

run_pscalasm_roundtrip_tests() {
    run_labels_and_proc_roundtrip_test
    run_extended_constants_test
    run_types_roundtrip_test
    run_shellfn_pointer_roundtrip_test
    run_charptr_pointer_roundtrip_test
    run_opaque_pointer_roundtrip_test
    run_pointer_const_symbol_roundtrip_test
    run_builtin_map_roundtrip_test
    run_procedure_upvalue_roundtrip_test
}

run_pscalasm_integration_tests() {
    run_real_source_roundtrip_tests
}

run_pscalasm_negative_tests() {
    run_negative_source_compile_tests
}

if ! require_tools; then
    harness_summary "pscalasm"
    if harness_exit_code; then
        exit 0
    fi
    exit 1
fi

run_pscalasm_unit_tests
run_pscalasm_roundtrip_tests
run_pscalasm_integration_tests
run_pscalasm_negative_tests

harness_summary "pscalasm"
if harness_exit_code; then
    exit 0
fi
exit 1
