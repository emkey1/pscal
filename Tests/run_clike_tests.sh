#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"
RUNNER_PY="$ROOT_DIR/Tests/tools/run_with_timeout.py"
TEST_TIMEOUT="${TEST_TIMEOUT:-25}"

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

# Use an isolated HOME to avoid interference from existing caches.
TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"
trap 'rm -rf "$TEST_HOME"' EXIT

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
  disasm_file="$SCRIPT_DIR/clike/$test_name.disasm"
  disasm_stdout=""
  disasm_stderr=""
  # Skip SDL-dependent clike tests unless RUN_SDL=1 forces them
  if [ "${RUN_SDL:-0}" != "1" ] && [ "${SDL_VIDEODRIVER:-}" = "dummy" ] && [ "$test_name" = "graphics" ]; then
    echo "Skipping $test_name (SDL dummy driver)"
    echo
    continue
  fi

  if [ -f "$disasm_file" ]; then
    disasm_stdout=$(mktemp)
    disasm_stderr=$(mktemp)
    set +e
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$CLIKE_BIN" --dump-bytecode-only "$src" \
      > "$disasm_stdout" 2> "$disasm_stderr"
    disasm_status=$?
    set -e
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$disasm_stderr" > "$disasm_stderr.clean"
    mv "$disasm_stderr.clean" "$disasm_stderr"
    perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$disasm_stderr" > "$disasm_stderr.clean"
    mv "$disasm_stderr.clean" "$disasm_stderr"
    rel_src="Tests/clike/$test_name.cl"
    sed -i.bak "s|$src|$rel_src|" "$disasm_stderr" 2>/dev/null || true
    rm -f "$disasm_stderr.bak"
    if [ $disasm_status -ne 0 ]; then
      echo "Disassembly run exited with $disasm_status: $test_name" >&2
      cat "$disasm_stderr"
      EXIT_CODE=1
    elif ! diff -u "$disasm_file" "$disasm_stderr"; then
      echo "Disassembly mismatch: $test_name" >&2
      EXIT_CODE=1
    fi
  fi

  # Skip network-labeled tests unless RUN_NET_TESTS=1
  if [ -f "$SCRIPT_DIR/clike/$test_name.net" ] && [ "${RUN_NET_TESTS:-0}" != "1" ]; then
    echo "Skipping $test_name (network test; set RUN_NET_TESTS=1 to enable)"
    echo
    continue
  fi

  echo "---- $test_name ----"

  server_pid=""
  server_script="$SCRIPT_DIR/clike/$test_name.net"
  if [ -f "$server_script" ] && [ "${RUN_NET_TESTS:-0}" = "1" ] && [ -s "$server_script" ]; then
    python3 "$server_script" &
    server_pid=$!
    # Detach the server process from job control so terminating it later does
    # not emit noisy "Terminated" messages that interfere with test output.
    disown "$server_pid" 2>/dev/null || true
    sleep 1
  fi

  set +e
  if [ -f "$in_file" ]; then
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$CLIKE_BIN" "$src" < "$in_file" > "$actual_out" 2> "$actual_err"
  else
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$CLIKE_BIN" "$src" > "$actual_out" 2> "$actual_err"
  fi
  run_status=$?
  set -e

  # Strip ANSI escape sequences (including OSC) that may be emitted by the VM
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_out" > "$actual_out.clean"
  mv "$actual_out.clean" "$actual_out"
  perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$actual_err" > "$actual_err.clean"
  mv "$actual_err.clean" "$actual_err"
  perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$actual_err" > "$actual_err.clean"
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

  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    sleep 0.2
    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi

  if [ -n "$disasm_stdout" ]; then rm -f "$disasm_stdout"; fi
  if [ -n "$disasm_stderr" ]; then rm -f "$disasm_stderr"; fi
  rm -f "$actual_out" "$actual_err"
  echo
  echo
done

# Basic cache reuse test for the CLike front end
echo "---- CacheReuseTest ----"
tmp_home=$(mktemp -d)
src_dir=$(mktemp -d)
cat > "$src_dir/CacheTest.cl" <<'EOF'
int main() {
    printf("first\\n");
    return 0;
}
EOF
sleep 1
set +e
(cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" CacheTest.cl > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  sleep 2
  set +e
  (cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" CacheTest.cl > "$tmp_home/out2" 2> "$tmp_home/err2")
  status2=$?
  set -e
  if [ $status2 -ne 0 ] || ! grep -q 'Loaded cached byte code' "$tmp_home/err2"; then
    echo "Cache reuse test failed: expected cached bytecode" >&2
    EXIT_CODE=1
  fi
else
  echo "Cache reuse test failed to run" >&2
  EXIT_CODE=1
fi
rm -rf "$tmp_home" "$src_dir"
echo

# Cache invalidation test when the CLike binary is newer than the cache
echo "---- CacheBinaryStalenessTest ----"
tmp_home=$(mktemp -d)
src_dir=$(mktemp -d)
cat > "$src_dir/BinaryTest.cl" <<'EOF'
int main() {
    printf("first\n");
    return 0;
}
EOF
sleep 1
set +e
(cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" BinaryTest.cl > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  sleep 2
  touch "$CLIKE_BIN"
  set +e
  (cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" BinaryTest.cl > "$tmp_home/out2" 2> "$tmp_home/err2")
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
