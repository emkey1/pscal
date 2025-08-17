#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
TINYC_BIN="$ROOT_DIR/build/bin/tinyc"

if [ ! -x "$TINYC_BIN" ]; then
  echo "tinyc binary not found at $TINYC_BIN" >&2
  exit 1
fi

EXIT_CODE=0

for src in "$SCRIPT_DIR"/tinyc/*.c; do
  test_name=$(basename "$src" .c)
  in_file="$SCRIPT_DIR/tinyc/$test_name.in"
  out_file="$SCRIPT_DIR/tinyc/$test_name.out"
  actual_file=$(mktemp)
  echo "---- $test_name ----"
  if [ -f "$in_file" ]; then
    "$TINYC_BIN" "$src" < "$in_file" > "$actual_file"
  else
    "$TINYC_BIN" "$src" > "$actual_file"
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
