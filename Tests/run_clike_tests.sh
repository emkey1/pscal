#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"
CLIKE_ARGS=(--no-cache)
RUNNER_PY="$ROOT_DIR/Tests/tools/run_with_timeout.py"
TEST_TIMEOUT="${TEST_TIMEOUT:-25}"

USE_TIMEOUT_RUNNER=1
if [ -r /etc/os-release ]; then
  if grep -Eiq '^(ID|ID_LIKE)=.*alpine' /etc/os-release; then
    USE_TIMEOUT_RUNNER=0
  fi
fi

run_clike_command() {
  if [ "$USE_TIMEOUT_RUNNER" = "1" ]; then
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$CLIKE_BIN" "${CLIKE_ARGS[@]}" "$@"
  else
    "$CLIKE_BIN" "${CLIKE_ARGS[@]}" "$@"
  fi
}

shift_mtime() {
  local path="$1"
  local delta="$2"
  python3 - "$path" "$delta" <<'PY'
import os
import sys
import time

path = sys.argv[1]
delta = float(sys.argv[2])
try:
    st = os.stat(path)
except FileNotFoundError:
    sys.exit(1)
now = time.time()
if delta >= 0:
    base = max(st.st_mtime, now)
else:
    base = min(st.st_mtime, now)
target = base + delta
if target < 0:
    target = 0.0
os.utime(path, (target, target))
PY
}

wait_for_ready_file() {
  local ready_file="$1"
  local timeout="$2"
  python3 - "$ready_file" "$timeout" <<'PY'
import os
import sys
import time

path = sys.argv[1]
timeout = float(sys.argv[2])
deadline = time.monotonic() + timeout
while time.monotonic() < deadline:
    if os.path.exists(path):
        sys.exit(0)
    time.sleep(0.05)
sys.exit(1)
PY
}

stop_server() {
  local pid="$1"
  if [ -z "$pid" ]; then
    return
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" 2>/dev/null || true
    return
  fi
  kill "$pid" 2>/dev/null || true
  for _ in {1..20}; do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return
    fi
    if ps -p "$pid" -o stat= 2>/dev/null | grep -q '^Z'; then
      wait "$pid" 2>/dev/null || true
      return
    fi
    sleep 0.05
  done
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

check_ext_builtin_dump() {
  local binary="$1"
  local label="$2"
  local tmp_out
  local tmp_err
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$binary" --dump-ext-builtins >"$tmp_out" 2>"$tmp_err"
  local status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "Failed to dump extended builtins for $label (exit $status)" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif [ -s "$tmp_err" ]; then
    echo "Unexpected stderr from $label --dump-ext-builtins:" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! python3 - "$tmp_out" <<'PY'
import sys

path = sys.argv[1]
seen = set()
with open(path, 'r', encoding='utf-8') as fh:
    for idx, raw_line in enumerate(fh, 1):
        line = raw_line.rstrip('\n')
        if not line:
            continue
        parts = line.split()
        if not parts:
            continue
        tag = parts[0]
        if tag == 'category':
            if len(parts) != 2:
                print(f"Invalid category line {idx}: {raw_line.rstrip()}", file=sys.stderr)
                sys.exit(1)
            seen.add(parts[1])
        elif tag == 'function':
            if len(parts) != 3:
                print(f"Invalid function line {idx}: {raw_line.rstrip()}", file=sys.stderr)
                sys.exit(1)
            if parts[1] not in seen:
                print(f"Function references unknown category on line {idx}: {raw_line.rstrip()}", file=sys.stderr)
                sys.exit(1)
        else:
            print(f"Unknown directive on line {idx}: {raw_line.rstrip()}", file=sys.stderr)
            sys.exit(1)
sys.exit(0)
PY
  then
    echo "Unexpected output format from $label --dump-ext-builtins" >&2
    cat "$tmp_out" >&2
    EXIT_CODE=1
  fi
  rm -f "$tmp_out" "$tmp_err"
}

exercise_clike_cli_smoke() {
  local fixture="$ROOT_DIR/Tests/tools/fixtures/cli_clike.cl"
  if [ ! -f "$fixture" ]; then
    echo "Clike CLI fixture missing at $fixture" >&2
    EXIT_CODE=1
    return
  fi

  local tmp_out tmp_err status

  echo "---- ClikeCLIVersion ----"
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$CLIKE_BIN" -v >"$tmp_out" 2>"$tmp_err"
  status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "clike -v exited with $status" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif [ -s "$tmp_err" ]; then
    echo "clike -v emitted stderr:" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! grep -q "latest tag:" "$tmp_out"; then
    echo "clike -v output missing latest tag:" >&2
    cat "$tmp_out" >&2
    EXIT_CODE=1
  fi
  rm -f "$tmp_out" "$tmp_err"
  echo

  echo "---- ClikeCLIAstJson ----"
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$CLIKE_BIN" --no-cache --dump-ast-json "$fixture" >"$tmp_out" 2>"$tmp_err"
  status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "clike --dump-ast-json failed with $status" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! grep -q "Dumping AST" "$tmp_err"; then
    echo "clike --dump-ast-json missing progress messages" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! grep -q '"node_type"' "$tmp_out"; then
    echo "clike --dump-ast-json produced unexpected stdout" >&2
    cat "$tmp_out" >&2
    EXIT_CODE=1
  fi
  rm -f "$tmp_out" "$tmp_err"
  echo

  echo "---- ClikeCLITrace ----"
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$CLIKE_BIN" --no-cache --vm-trace-head=3 "$fixture" >"$tmp_out" 2>"$tmp_err"
  status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "clike --vm-trace-head exited with $status" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! grep -q "\[VM-TRACE\]" "$tmp_err"; then
    echo "clike --vm-trace-head did not emit trace output" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  fi
  rm -f "$tmp_out" "$tmp_err"
  echo
}

has_ext_builtin_category() {
  local binary="$1"
  local category="$2"
  set +e
  "$binary" --dump-ext-builtins | grep -Ei "^category[[:space:]]+${category}\$" >/dev/null
  local status=$?
  set -e
  return $status
=
}

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

check_ext_builtin_dump "$CLIKE_BIN" clike
exercise_clike_cli_smoke

if has_ext_builtin_category "$CLIKE_BIN" sqlite; then
  CLIKE_SQLITE_AVAILABLE=1
else
  CLIKE_SQLITE_AVAILABLE=0
fi

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

  if [ -f "$SCRIPT_DIR/clike/$test_name.sqlite" ] && [ "$CLIKE_SQLITE_AVAILABLE" -ne 1 ]; then
    echo "Skipping $test_name (SQLite builtins disabled)"
    echo
    continue
  fi

  if [ -f "$disasm_file" ]; then
    disasm_stdout=$(mktemp)
    disasm_stderr=$(mktemp)
    set +e
    run_clike_command --dump-bytecode-only "$src" \
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
  server_ready_file=""
  if [ -f "$server_script" ] && [ "${RUN_NET_TESTS:-0}" = "1" ] && [ -s "$server_script" ]; then
    server_ready_file=$(mktemp)
    rm -f "$server_ready_file"
    PSCAL_NET_READY_FILE="$server_ready_file" python3 "$server_script" &
    server_pid=$!
    # Detach the server process from job control so terminating it later does
    # not emit noisy "Terminated" messages that interfere with test output.
    disown "$server_pid" 2>/dev/null || true
    if ! wait_for_ready_file "$server_ready_file" 5; then
      echo "Failed to start server for $test_name" >&2
      EXIT_CODE=1
      if [ -n "$server_pid" ]; then
        stop_server "$server_pid"
        server_pid=""
      fi
    fi
  fi

  set +e
  if [ -f "$in_file" ]; then
    run_clike_command "$src" < "$in_file" > "$actual_out" 2> "$actual_err"
  else
    run_clike_command "$src" > "$actual_out" 2> "$actual_err"
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
    stop_server "$server_pid"
    server_pid=""
  fi
  if [ -n "${server_ready_file:-}" ]; then
    rm -f "$server_ready_file"
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
shift_mtime "$src_dir/CacheTest.cl" -5
set +e
(cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" CacheTest.cl > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
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
shift_mtime "$src_dir/BinaryTest.cl" -5
set +e
(cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" BinaryTest.cl > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  shift_mtime "$CLIKE_BIN" 5
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
