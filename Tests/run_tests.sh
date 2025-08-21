#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PASCAL_BIN="$ROOT_DIR/build/bin/pascal"

if [ ! -x "$PASCAL_BIN" ]; then
  echo "pascal binary not found at $PASCAL_BIN" >&2
  exit 1
fi

CRT_UNIT="$ROOT_DIR/lib/pascal/crtvt.pl"
PASCAL_LIB_ROOT="${PASCAL_LIB_DIR:-$ROOT_DIR/test_pscal_lib}"
PASCAL_LIB_DIR="$PASCAL_LIB_ROOT/pascal"
export PASCAL_LIB_DIR
mkdir -p "$PASCAL_LIB_ROOT"
cp -R "$ROOT_DIR/lib/." "$PASCAL_LIB_ROOT/"
if [ -f "$CRT_UNIT" ]; then
  cp "$CRT_UNIT" "$PASCAL_LIB_DIR/crt.pl"
fi

EXIT_CODE=0

for src in "$SCRIPT_DIR"/Pascal/*.p; do
  test_name=$(basename "$src" .p)
  in_file="$SCRIPT_DIR/Pascal/$test_name.in"
  out_file="$SCRIPT_DIR/Pascal/$test_name.out"
  err_file="$SCRIPT_DIR/Pascal/$test_name.err"
  actual_out=$(mktemp)
  actual_err=$(mktemp)
  echo "---- $test_name ----"

  set +e
  if [ -f "$in_file" ]; then
    "$PASCAL_BIN" "$src" < "$in_file" > "$actual_out" 2> "$actual_err"
  else
    "$PASCAL_BIN" "$src" > "$actual_out" 2> "$actual_err"
  fi
  run_status=$?
  set -e

  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g' "$actual_out" > "$actual_out.clean"
  mv "$actual_out.clean" "$actual_out"
  # Normalize dynamic values that would otherwise cause spurious diffs
  perl -pe 's/pid=\d+/pid=PID/' "$actual_out" > "$actual_out.norm"
  mv "$actual_out.norm" "$actual_out"
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g' "$actual_err" | grep -v '^Compilation successful\.' > "$actual_err.clean" || true
  mv "$actual_err.clean" "$actual_err"
  head -n 2 "$actual_err" > "$actual_err.trim"
  mv "$actual_err.trim" "$actual_err"

  # Ignore any stdout files accidentally created during failing tests
  if [ -f "$err_file" ]; then
    rm -f "$out_file"
  fi

  if [ -f "$out_file" ]; then
    if ! diff -u "$out_file" "$actual_out"; then
      echo "Test failed (stdout mismatch): $test_name" >&2
      EXIT_CODE=1
    fi
  elif [ -s "$actual_out" ] && [ ! -f "$err_file" ]; then
    echo "Test produced unexpected stdout: $test_name" >&2
    cat "$actual_out"
    EXIT_CODE=1
  fi

  if [ -f "$err_file" ]; then
    if ! diff -u "$err_file" "$actual_err"; then
      echo "Test failed (stderr mismatch): $test_name" >&2
      EXIT_CODE=1
    fi
    if [ $run_status -eq 0 ]; then
      echo "Test expected failure but exited with 0: $test_name" >&2
      EXIT_CODE=1
    fi
  else
    if [ $run_status -ne 0 ]; then
      echo "Test exited with $run_status: $test_name" >&2
      cat "$actual_err"
      EXIT_CODE=1
    elif [ -s "$actual_err" ]; then
      echo "Unexpected stderr output in $test_name:" >&2
      cat "$actual_err"
      EXIT_CODE=1
    fi
  fi

  rm -f "$actual_out" "$actual_err"
  echo
  echo

done

rm -f "$ROOT_DIR/edge_input.txt" "$ROOT_DIR/readln_test.txt"

exit $EXIT_CODE
