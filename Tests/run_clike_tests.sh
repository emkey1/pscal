#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"

# Detect SDL enabled and set dummy drivers by default unless RUN_SDL=1
if grep -q '^SDL:BOOL=ON$' "$ROOT_DIR/build/CMakeCache.txt" 2>/dev/null; then
  if [ "${RUN_SDL:-0}" != "1" ]; then
    export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
    export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
  fi
fi

if [ ! -x "$CLIKE_BIN" ]; then
  echo "clike binary not found at $CLIKE_BIN" >&2
  exit 1
fi

# Ensure tests run from repository root so relative paths resolve correctly
cd "$ROOT_DIR"

EXIT_CODE=0

for src in "$SCRIPT_DIR"/clike/*.cl; do
  test_name=$(basename "$src" .cl)
  in_file="$SCRIPT_DIR/clike/$test_name.in"
  out_file="$SCRIPT_DIR/clike/$test_name.out"
  err_file="$SCRIPT_DIR/clike/$test_name.err"
  actual_out=$(mktemp)
  actual_err=$(mktemp)
  # Skip SDL-dependent clike tests unless RUN_SDL=1 forces them
  if [ "${RUN_SDL:-0}" != "1" ] && [ "${SDL_VIDEODRIVER:-}" = "dummy" ] && [ "$test_name" = "graphics" ]; then
    echo "Skipping $test_name (SDL dummy driver)"
    echo
    continue
  fi

  echo "---- $test_name ----"

  set +e
  if [ -f "$in_file" ]; then
    "$CLIKE_BIN" "$src" < "$in_file" > "$actual_out" 2> "$actual_err"
  else
    "$CLIKE_BIN" "$src" > "$actual_out" 2> "$actual_err"
  fi
  run_status=$?
  set -e

  # Strip ANSI escape sequences (including OSC) that may be emitted by the VM
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_out" > "$actual_out.clean"
  mv "$actual_out.clean" "$actual_out"
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_err" > "$actual_err.clean"
  mv "$actual_err.clean" "$actual_err"

  # Keep only the first two lines of stderr for deterministic comparisons
  head -n 2 "$actual_err" > "$actual_err.trim"
  mv "$actual_err.trim" "$actual_err"

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

exit $EXIT_CODE
