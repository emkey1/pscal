#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"
CLIKE_ARGS=(--no-cache)
JSON2BC_BIN="$ROOT_DIR/build/bin/pscaljson2bc"
PSCALVM_BIN="$ROOT_DIR/build/bin/pscalvm"

if [ ! -x "$CLIKE_BIN" ]; then
  echo "clike binary not found at $CLIKE_BIN" >&2
  exit 1
fi
if [ ! -x "$JSON2BC_BIN" ]; then
  echo "pscaljson2bc binary not found at $JSON2BC_BIN" >&2
  exit 1
fi
if [ ! -x "$PSCALVM_BIN" ]; then
  echo "pscalvm binary not found at $PSCALVM_BIN" >&2
  exit 1
fi

# Use an isolated HOME to avoid cache interference
TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"
trap 'rm -rf "$TEST_HOME"' EXIT

cd "$ROOT_DIR"

EXIT_CODE=0

for src in "$SCRIPT_DIR"/json2bc/*.cl; do
  test_name=$(basename "$src" .cl)
  out_file="$SCRIPT_DIR/json2bc/$test_name.out"
  actual_out=$(mktemp)
  actual_err=$(mktemp)
  tmp_bc=$(mktemp)

  echo "---- json2bc:$test_name ----"
  set +e
  # Pipe AST JSON from clike to pscaljson2bc, then run the VM
  "$CLIKE_BIN" "${CLIKE_ARGS[@]}" --dump-ast-json "$src" | "$JSON2BC_BIN" -o "$tmp_bc" > "$actual_err" 2>&1
  run_status=$?
  if [ $run_status -ne 0 ]; then
    echo "json2bc compile failed for $test_name" >&2
    cat "$actual_err" >&2
    EXIT_CODE=1
    rm -f "$tmp_bc" "$actual_out" "$actual_err"
    set -e
    continue
  fi
  "$PSCALVM_BIN" "$tmp_bc" > "$actual_out" 2>> "$actual_err"
  vm_status=$?
  set -e

  # Strip ANSI sequences from outputs
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_out" > "$actual_out.clean"
  mv "$actual_out.clean" "$actual_out"
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_err" > "$actual_err.clean"
  mv "$actual_err.clean" "$actual_err"

  if [ -f "$out_file" ]; then
    if ! diff -u "$out_file" "$actual_out"; then
      echo "json2bc test failed (stdout mismatch): $test_name" >&2
      EXIT_CODE=1
    fi
  elif [ -s "$actual_out" ]; then
    echo "json2bc test produced unexpected stdout: $test_name" >&2
    cat "$actual_out"
    EXIT_CODE=1
  fi

  if [ $vm_status -ne 0 ] || [ -s "$actual_err" ]; then
    echo "json2bc test had unexpected errors ($test_name):" >&2
    cat "$actual_err" >&2
    EXIT_CODE=1
  fi

  rm -f "$tmp_bc" "$actual_out" "$actual_err"
  echo
done

exit $EXIT_CODE

