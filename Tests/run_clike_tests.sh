#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"

if [ ! -x "$CLIKE_BIN" ]; then
  echo "clike binary not found at $CLIKE_BIN" >&2
  exit 1
fi

EXIT_CODE=0

for src in "$SCRIPT_DIR"/clike/*.c; do
  test_name=$(basename "$src" .c)
  in_file="$SCRIPT_DIR/clike/$test_name.in"
  out_file="$SCRIPT_DIR/clike/$test_name.out"
  actual_file=$(mktemp)
  echo "---- $test_name ----"
  if [ -f "$in_file" ]; then
    "$CLIKE_BIN" "$src" < "$in_file" > "$actual_file"
  else
    "$CLIKE_BIN" "$src" > "$actual_file"
  fi
  # Strip ANSI escape sequences that may be emitted by the VM
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g' "$actual_file" > "$actual_file.clean"
  mv "$actual_file.clean" "$actual_file"
  if ! diff -u "$out_file" "$actual_file"; then
    echo "Test failed: $test_name" >&2
    EXIT_CODE=1
  fi
  rm -f "$actual_file"
  echo
  echo

done

exit $EXIT_CODE
