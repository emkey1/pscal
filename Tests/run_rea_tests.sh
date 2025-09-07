#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd -P)"
REA_BIN="$ROOT_DIR/build/bin/rea"
RUNNER_PY="$ROOT_DIR/Tests/tools/run_with_timeout.py"
TEST_TIMEOUT="${TEST_TIMEOUT:-25}"

if [ ! -x "$REA_BIN" ]; then
  echo "rea binary not found at $REA_BIN" >&2
  exit 1
fi

cd "$ROOT_DIR"
EXIT_CODE=0

for src in "$SCRIPT_DIR"/rea/*.rea; do
  test_name=$(basename "$src" .rea)
  in_file="$SCRIPT_DIR/rea/$test_name.in"
  out_file="$SCRIPT_DIR/rea/$test_name.out"
  err_file="$SCRIPT_DIR/rea/$test_name.err"
  src_rel=${src#$ROOT_DIR/}
  args_file="$SCRIPT_DIR/rea/$test_name.args"
  if [ -f "$args_file" ]; then
    if ! read -r args < "$args_file"; then
      args=""
    fi
  else
    args="--dump-bytecode-only"
  fi
  actual_out=$(mktemp)
  actual_err=$(mktemp)

  echo "---- $test_name ----"
  set +e
  if [ -f "$in_file" ]; then
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$REA_BIN" $args "$src_rel" < "$in_file" > "$actual_out" 2> "$actual_err"
  else
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$REA_BIN" $args "$src_rel" > "$actual_out" 2> "$actual_err"
  fi
  run_status=$?
  set -e

  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_out" > "$actual_out.clean"
  mv "$actual_out.clean" "$actual_out"
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_err" > "$actual_err.clean"
  mv "$actual_err.clean" "$actual_err"

  if [ -f "$out_file" ]; then
    if ! diff -u "$out_file" "$actual_out"; then
      echo "Test failed (stdout mismatch): $test_name" >&2
      EXIT_CODE=1
    fi
  elif [ -s "$actual_out" ]; then
    echo "Test produced unexpected stdout: $test_name" >&2
    cat "$actual_out"
    EXIT_CODE=1
  fi

  if [ -f "$err_file" ]; then
    if ! diff -u "$err_file" "$actual_err"; then
      echo "Test failed (stderr mismatch): $test_name" >&2
      EXIT_CODE=1
    fi
  elif [ -s "$actual_err" ]; then
    echo "Unexpected stderr output in $test_name:" >&2
    cat "$actual_err"
    EXIT_CODE=1
  fi

  if [ $run_status -ne 0 ]; then
    echo "Test exited with $run_status: $test_name" >&2
    EXIT_CODE=1
  fi

  rm -f "$actual_out" "$actual_err"
  echo
  echo
 done

exit $EXIT_CODE
