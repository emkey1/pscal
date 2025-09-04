#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PASCAL_BIN="$ROOT_DIR/build/bin/pascal"

if [ ! -x "$PASCAL_BIN" ]; then
  echo "pascal binary not found at $PASCAL_BIN" >&2
  exit 1
fi

# Use an isolated HOME to avoid interference from any existing caches.
TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"
trap 'rm -rf "$TEST_HOME"' EXIT

CRT_UNIT="$ROOT_DIR/lib/pascal/crtvt.pl"
PASCAL_LIB_ROOT="${PASCAL_LIB_DIR:-$ROOT_DIR/test_pscal_lib}"
PASCAL_LIB_DIR="$PASCAL_LIB_ROOT/pascal"
export PASCAL_LIB_DIR
export PSCAL_LIB_DIR="$PASCAL_LIB_DIR"
mkdir -p "$PASCAL_LIB_ROOT"
cp -R "$ROOT_DIR/lib/." "$PASCAL_LIB_ROOT/"
if [ -f "$CRT_UNIT" ]; then
  cp "$CRT_UNIT" "$PASCAL_LIB_DIR/crt.pl"
fi

if grep -q '^SDL:BOOL=ON$' "$ROOT_DIR/build/CMakeCache.txt" 2>/dev/null; then
  SDL_ENABLED=1
else
  SDL_ENABLED=0
fi

# Headless-friendly defaults unless RUN_SDL=1 is explicitly set by the caller
if [ "${RUN_SDL:-0}" != "1" ] && [ "$SDL_ENABLED" -eq 1 ]; then
  export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
  export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
fi

EXIT_CODE=0

# Iterate over Pascal test sources (files without extensions)
for src in "$SCRIPT_DIR"/Pascal/*; do
  test_name=$(basename "$src")
  if [[ "$test_name" == *.* ]]; then
    continue
  fi
  # Skip SDL-dependent test unless RUN_SDL=1 forces it
  if [ "${RUN_SDL:-0}" != "1" ] && { [ "$SDL_ENABLED" -eq 0 ] || [ "${SDL_VIDEODRIVER:-}" = "dummy" ]; } && [ "$test_name" = "SDLFeaturesTest" ]; then
    echo "Skipping $test_name (SDL disabled)"
    echo
    continue
  fi

  # Skip network-dependent test when running on GitHub Actions
  if [ "$test_name" = "SocketSendReceiveTest" ] && [ "${GITHUB_ACTIONS:-}" = "true" ]; then
    echo "Skipping $test_name (networking disabled on GitHub Actions)"
    echo
    continue
  fi

  in_file="$SCRIPT_DIR/Pascal/$test_name.in"
  out_file="$SCRIPT_DIR/Pascal/$test_name.out"
  err_file="$SCRIPT_DIR/Pascal/$test_name.err"
  actual_out=$(mktemp)
  actual_err=$(mktemp)

  echo "---- $test_name ----"

  set +e
  if [ "$test_name" = "SDLFeaturesTest" ]; then
    (cd "$SCRIPT_DIR" && printf 'Q\n' | "$PASCAL_BIN" "Pascal/$test_name" > "$actual_out" 2> "$actual_err")
  elif [ -f "$in_file" ]; then
    (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "Pascal/$test_name" < "$in_file" > "$actual_out" 2> "$actual_err")
  else
    (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "Pascal/$test_name" > "$actual_out" 2> "$actual_err")
  fi
  run_status=$?
  set -e

  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g; s/\e\][^\a]*\a//g' "$actual_out" > "$actual_out.clean" && mv "$actual_out.clean" "$actual_out"
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g; s/\e\][^\a]*\a//g' "$actual_err" > "$actual_err.clean" && mv "$actual_err.clean" "$actual_err"
  perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$actual_err" > "$actual_err.clean" && mv "$actual_err.clean" "$actual_err"
  perl -ne 'print unless /Warning: user-defined .* overrides builtin/' "$actual_err" > "$actual_err.clean" && mv "$actual_err.clean" "$actual_err"
  head -n 2 "$actual_err" > "$actual_err.trim" && mv "$actual_err.trim" "$actual_err"
  perl -pe 's/pid=[0-9]+/pid=<PID>/g' "$actual_out" > "$actual_out.clean" && mv "$actual_out.clean" "$actual_out"
  perl -ne 'print unless /^[0-9]{4}/' "$actual_out" > "$actual_out.clean" && mv "$actual_out.clean" "$actual_out"

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

  rm -f "$SCRIPT_DIR/Pascal/$test_name.dbg"
  rm -f "$actual_out" "$actual_err"
  echo
  echo

done

# Regression test to ensure cached bytecode is invalidated when the
# source file shares the same timestamp as the cache entry.
echo "---- CacheStalenessTest ----"
tmp_home=$(mktemp -d)
src_dir=$(mktemp -d)
src_file="$src_dir/CacheStalenessTest"
cat > "$src_file" <<'EOF'
program CacheStalenessTest;
begin
  writeln('first');
end.
EOF

# Ensure the cache entry's timestamp exceeds the source file's mtime.
sleep 1

# First run to populate the cache
set +e
(cd "$src_dir" && HOME="$tmp_home" "$PASCAL_BIN" "CacheStalenessTest" > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e

if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  # Second run should use the cache
  sleep 2
  set +e
  (cd "$src_dir" && HOME="$tmp_home" "$PASCAL_BIN" "CacheStalenessTest" > "$tmp_home/out_cache" 2> "$tmp_home/err_cache")
  status_cache=$?
  set -e

  if [ $status_cache -ne 0 ] || ! grep -q 'Loaded cached byte code' "$tmp_home/err_cache" || ! grep -q 'first' "$tmp_home/out_cache"; then
    echo "Cache reuse test failed: expected cached bytecode" >&2
    EXIT_CODE=1
  fi

  cache_file=$(find "$tmp_home/.pscal_cache" -name '*.bc' | head -n 1)

  cat > "$src_file" <<'EOF'
program CacheStalenessTest;
begin
  writeln('second');
end.
EOF
  # Copy the cache file's timestamp to the source file so their mtimes match.
  # Using "touch -r" avoids reliance on GNU-specific options like
  # "stat -c" or "touch -d", improving portability (e.g., macOS).
  touch -r "$cache_file" "$src_file"

  set +e
  (cd "$src_dir" && HOME="$tmp_home" "$PASCAL_BIN" "CacheStalenessTest" > "$tmp_home/out2" 2> "$tmp_home/err2")
  status2=$?
  set -e

  if [ $status2 -ne 0 ] || ! grep -q 'second' "$tmp_home/out2"; then
    echo "Cache staleness test failed: expected recompilation" >&2
    EXIT_CODE=1
  fi
else
  echo "Cache staleness test failed to run" >&2
  EXIT_CODE=1
fi

rm -rf "$tmp_home" "$src_dir"
echo

exit $EXIT_CODE
