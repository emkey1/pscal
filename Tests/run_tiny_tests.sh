#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
TINY_BIN="$ROOT_DIR/tools/tiny"
VM_BIN="$ROOT_DIR/build/bin/pscalvm"

if [ ! -x "$TINY_BIN" ]; then
  echo "tiny script not found at $TINY_BIN" >&2
  exit 1
fi

if [ ! -x "$VM_BIN" ]; then
  echo "pscalvm binary not found at $VM_BIN" >&2
  exit 1
fi

EXIT_CODE=0

for src in "$SCRIPT_DIR"/tiny/*.tiny; do
  test_name=$(basename "$src" .tiny)
  in_file="$SCRIPT_DIR/tiny/$test_name.in"
  out_file="$SCRIPT_DIR/tiny/$test_name.out"
  bytecode_file=$(mktemp "$SCRIPT_DIR/$test_name.XXXXXX.pbc")
  actual_out=$(mktemp)
  echo "---- $test_name ----"

  if ! "$TINY_BIN" "$src" "$bytecode_file"; then
    echo "Compilation failed: $test_name" >&2
    EXIT_CODE=1
    rm -f "$bytecode_file" "$actual_out"
    continue
  fi

  if [ -f "$in_file" ]; then
    "$VM_BIN" "$bytecode_file" < "$in_file" > "$actual_out"
  else
    "$VM_BIN" "$bytecode_file" > "$actual_out"
  fi
  run_status=$?

  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g' "$actual_out" > "$actual_out.clean"
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

  rm -f "$bytecode_file" "$actual_out"
  echo
  echo

done

exit $EXIT_CODE
