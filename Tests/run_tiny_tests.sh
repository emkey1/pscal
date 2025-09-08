#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
TINY_BIN="$ROOT_DIR/tools/tiny"
VM_BIN="$ROOT_DIR/build/bin/pscalvm"
DCOMP_BIN="$ROOT_DIR/build/bin/pscald"

if [ ! -x "$TINY_BIN" ]; then
  echo "tiny script not found at $TINY_BIN" >&2
  exit 1
fi

if [ ! -x "$VM_BIN" ]; then
  echo "pscalvm binary not found at $VM_BIN" >&2
  exit 1
fi
if [ ! -x "$DCOMP_BIN" ]; then
  echo "pscald binary not found at $DCOMP_BIN; skipping disassembly checks." >&2
  DCOMP_BIN=""
fi

EXIT_CODE=0

for src in "$SCRIPT_DIR"/tiny/*.tiny; do
  test_name=$(basename "$src" .tiny)
  in_file="$SCRIPT_DIR/tiny/$test_name.in"
  out_file="$SCRIPT_DIR/tiny/$test_name.out"
  disasm_file="$SCRIPT_DIR/tiny/$test_name.disasm"
  bytecode_file="$SCRIPT_DIR/tiny/$test_name.pbc"
  actual_out=$(mktemp)
  actual_disasm=$(mktemp)
  echo "---- $test_name ----"

  if ! "$TINY_BIN" "$src" "$bytecode_file"; then
    echo "Compilation failed: $test_name" >&2
    EXIT_CODE=1
    rm -f "$bytecode_file" "$actual_out" "$actual_disasm"
    continue
  fi

  if [ -f "$in_file" ]; then
    "$VM_BIN" "$bytecode_file" < "$in_file" > "$actual_out"
  else
    "$VM_BIN" "$bytecode_file" > "$actual_out"
  fi
  run_status=$?

  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g; s/\e\].*?\a//g' "$actual_out" > "$actual_out.clean"
  mv "$actual_out.clean" "$actual_out"

  if [ -f "$out_file" ]; then
    if ! diff -u "$out_file" "$actual_out"; then
      echo "Test failed: $test_name" >&2
      EXIT_CODE=1
    fi
  elif [ -s "$actual_out" ]; then
    echo "Unexpected output for $test_name" >&2
    cat "$actual_out"
    EXIT_CODE=1
  fi

  if [ $run_status -ne 0 ]; then
    echo "pscalvm exited with $run_status: $test_name" >&2
    EXIT_CODE=1
  fi

  if [ -n "$DCOMP_BIN" ] && [ -f "$disasm_file" ]; then
    "$DCOMP_BIN" "$bytecode_file" 2> "$actual_disasm"
    # Normalize absolute path in header to expected relative path
    # Use portable sed -i invocation that works on both GNU and BSD sed.
    sed -i.bak "s|$bytecode_file|tiny/$test_name.pbc|" "$actual_disasm"
    rm -f "$actual_disasm.bak"
    if ! diff -u "$disasm_file" "$actual_disasm"; then
      echo "Disassembly mismatch: $test_name" >&2
      EXIT_CODE=1
    fi
  fi

  rm -f "$bytecode_file" "$actual_out" "$actual_disasm"
  echo
  echo

done

exit $EXIT_CODE
