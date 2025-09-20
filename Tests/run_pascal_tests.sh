#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PASCAL_BIN="$ROOT_DIR/build/bin/pascal"
PASCAL_ARGS=(--no-cache)

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

  # Skip network-labeled tests unless RUN_NET_TESTS=1
  if [ -f "$SCRIPT_DIR/Pascal/$test_name.net" ] && [ "${RUN_NET_TESTS:-0}" != "1" ]; then
    echo "Skipping $test_name (network test; set RUN_NET_TESTS=1 to enable)"
    echo
    continue
  fi

  in_file="$SCRIPT_DIR/Pascal/$test_name.in"
  out_file="$SCRIPT_DIR/Pascal/$test_name.out"
  err_file="$SCRIPT_DIR/Pascal/$test_name.err"
  actual_out=$(mktemp)
  actual_err=$(mktemp)
  disasm_file="$SCRIPT_DIR/Pascal/$test_name.disasm"
  disasm_stdout=""
  disasm_stderr=""

  echo "---- $test_name ----"

  if [ -f "$disasm_file" ]; then
    disasm_stdout=$(mktemp)
    disasm_stderr=$(mktemp)
    set +e
    (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "${PASCAL_ARGS[@]}" --dump-bytecode-only "Pascal/$test_name") \
      > "$disasm_stdout" 2> "$disasm_stderr"
    disasm_status=$?
    set -e
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g; s/\e\][^\a]*\a//g' "$disasm_stderr" > "$disasm_stderr.clean" && mv "$disasm_stderr.clean" "$disasm_stderr"
    perl -ne 'print unless /^Loaded cached byte code/' "$disasm_stderr" > "$disasm_stderr.clean" && mv "$disasm_stderr.clean" "$disasm_stderr"
    if [ $disasm_status -ne 0 ]; then
      echo "Disassembly run exited with $disasm_status: $test_name" >&2
      cat "$disasm_stderr"
      EXIT_CODE=1
    elif ! diff -u "$disasm_file" "$disasm_stderr"; then
      echo "Disassembly mismatch: $test_name" >&2
      EXIT_CODE=1
    fi
  fi

  set +e
  if [ "$test_name" = "SDLFeaturesTest" ]; then
    (cd "$SCRIPT_DIR" && printf 'Q\n' | "$PASCAL_BIN" "${PASCAL_ARGS[@]}" "Pascal/$test_name" > "$actual_out" 2> "$actual_err")
  elif [ -f "$in_file" ]; then
    (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "${PASCAL_ARGS[@]}" "Pascal/$test_name" < "$in_file" > "$actual_out" 2> "$actual_err")
  else
    (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "${PASCAL_ARGS[@]}" "Pascal/$test_name" > "$actual_out" 2> "$actual_err")
  fi
  run_status=$?
  set -e

  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g; s/\e\][^\a]*\a//g' "$actual_out" > "$actual_out.clean" && mv "$actual_out.clean" "$actual_out"
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g; s/\e\][^\a]*\a//g' "$actual_err" > "$actual_err.clean" && mv "$actual_err.clean" "$actual_err"
  perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$actual_err" > "$actual_err.clean" && mv "$actual_err.clean" "$actual_err"
  perl -ne 'print unless /Warning: user-defined .* overrides builtin/' "$actual_err" > "$actual_err.clean" && mv "$actual_err.clean" "$actual_err"
  perl -ne 'print unless /Compiler warning: assigning .* may lose precision/' "$actual_err" > "$actual_err.clean" && mv "$actual_err.clean" "$actual_err"
  head -n 2 "$actual_err" > "$actual_err.trim" && mv "$actual_err.trim" "$actual_err"
  perl -pe 's/pid=[0-9]+/pid=<PID>/g' "$actual_out" > "$actual_out.clean" && mv "$actual_out.clean" "$actual_out"
  # Remove ISO-like date lines (e.g., 2024-09-01 ...), not generic 4-digit prefixes
  perl -ne 'print unless /^[12][0-9]{3}-[01][0-9]-[0-3][0-9]/' "$actual_out" > "$actual_out.clean" && mv "$actual_out.clean" "$actual_out"

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
  if [ -n "$disasm_stdout" ]; then rm -f "$disasm_stdout"; fi
  if [ -n "$disasm_stderr" ]; then rm -f "$disasm_stderr"; fi
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

  src_real=$(realpath "$src_file")
  if ! python3 - "$cache_file" "$src_real" <<'PY'
import sys, struct, os
cf, expected = sys.argv[1], sys.argv[2]
with open(cf, "rb") as f:
    f.read(8)
    data = f.read(4)
    if len(data) != 4:
        sys.exit(1)
    stored = struct.unpack("<i", data)[0]
    if stored >= 0:
        sys.exit(1)
    path = f.read(-stored).decode("utf-8")
    sys.exit(0 if os.path.realpath(path) == os.path.realpath(expected) else 1)
PY
  then
    echo "Cache file does not embed source path" >&2
    EXIT_CODE=1
  fi

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
echo

# Cache invalidation test when the Pascal binary is newer than the cache
echo "---- CacheBinaryStalenessTest ----"
tmp_home=$(mktemp -d)
src_dir=$(mktemp -d)
cat > "$src_dir/BinaryTest" <<'EOF'
program BinaryTest;
begin
  writeln('first');
end.
EOF

# Ensure cache timestamp precedes binary update
sleep 1
set +e
(cd "$src_dir" && HOME="$tmp_home" "$PASCAL_BIN" BinaryTest > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  sleep 2
  touch "$PASCAL_BIN"
  set +e
  (cd "$src_dir" && HOME="$tmp_home" "$PASCAL_BIN" BinaryTest > "$tmp_home/out2" 2> "$tmp_home/err2")
  status2=$?
  set -e
  if [ $status2 -ne 0 ] || grep -q 'Loaded cached byte code' "$tmp_home/err2"; then
    echo "Cache binary staleness test failed: expected cache invalidation" >&2
    EXIT_CODE=1
  fi
else
  echo "Cache binary staleness test failed to run" >&2
  EXIT_CODE=1
fi
rm -rf "$tmp_home" "$src_dir"
echo

exit $EXIT_CODE
