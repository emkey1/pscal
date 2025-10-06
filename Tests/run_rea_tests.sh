#!/usr/bin/env bash
set -euo pipefail

# Resolve the script directory and repository root using realpath to ensure
# the canonical filesystem casing is preserved even when the script is invoked
# via a differently-cased path (e.g. "tests" vs "Tests").  macOS filesystems
# are typically case-insensitive, which previously led to lowercase paths being
# passed to the compiler and causing test diffs.
SCRIPT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(os.path.dirname(sys.argv[1])))' "${BASH_SOURCE[0]}")"
ROOT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$SCRIPT_DIR/..")"
REA_BIN="$ROOT_DIR/build/bin/rea"
RUNNER_PY="$ROOT_DIR/Tests/tools/run_with_timeout.py"
TEST_TIMEOUT="${TEST_TIMEOUT:-25}"

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
groups = {}
DEFAULT_GROUP = 'default'
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
            groups.setdefault(parts[1], set())
        elif tag == 'group':
            if len(parts) != 3:
                print(f"Invalid group line {idx}: {raw_line.rstrip()}", file=sys.stderr)
                sys.exit(1)
            if parts[1] not in seen:
                print(f"Group references unknown category on line {idx}: {raw_line.rstrip()}", file=sys.stderr)
                sys.exit(1)
            groups.setdefault(parts[1], set()).add(parts[2])
        elif tag == 'function':
            if len(parts) != 4:
                print(f"Invalid function line {idx}: {raw_line.rstrip()}", file=sys.stderr)
                sys.exit(1)
            category, group = parts[1], parts[2]
            if category not in seen:
                print(f"Function references unknown category on line {idx}: {raw_line.rstrip()}", file=sys.stderr)
                sys.exit(1)
            if group != DEFAULT_GROUP and group not in groups.get(category, set()):
                print(f"Function references unknown group on line {idx}: {raw_line.rstrip()}", file=sys.stderr)
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

exercise_rea_cli_smoke() {
  local fixture="$ROOT_DIR/Tests/tools/fixtures/cli_rea.rea"
  if [ ! -f "$fixture" ]; then
    echo "Rea CLI fixture missing at $fixture" >&2
    EXIT_CODE=1
    return
  fi

  local tmp_out tmp_err status

  echo "---- ReaCLIVersion ----"
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$REA_BIN" -v >"$tmp_out" 2>"$tmp_err"
  status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "rea -v exited with $status" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif [ -s "$tmp_err" ]; then
    echo "rea -v emitted stderr:" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! grep -q "latest tag:" "$tmp_out"; then
    echo "rea -v output missing latest tag:" >&2
    cat "$tmp_out" >&2
    EXIT_CODE=1
  fi
  rm -f "$tmp_out" "$tmp_err"
  echo

  echo "---- ReaCLIStrict ----"
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$REA_BIN" --no-cache --strict --dump-bytecode-only "$fixture" >"$tmp_out" 2>"$tmp_err"
  status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "rea --strict --dump-bytecode-only exited with $status" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! grep -q "Compiling Main Program AST to Bytecode" "$tmp_err"; then
    echo "rea --strict --dump-bytecode-only missing disassembly banner" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  fi
  rm -f "$tmp_out" "$tmp_err"
  echo

  echo "---- ReaCLINoRun ----"
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$REA_BIN" --no-cache --no-run "$fixture" >"$tmp_out" 2>"$tmp_err"
  status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "rea --no-run exited with $status" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif [ -s "$tmp_out" ]; then
    echo "rea --no-run produced unexpected stdout" >&2
    cat "$tmp_out"
    EXIT_CODE=1
  elif ! grep -q "Compilation successful" "$tmp_err"; then
    echo "rea --no-run missing compilation banner" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif grep -q -- "--- executing Program" "$tmp_err"; then
    echo "rea --no-run should not announce VM execution" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  fi
  rm -f "$tmp_out" "$tmp_err"
  echo

  echo "---- ReaCLITrace ----"
  tmp_out=$(mktemp)
  tmp_err=$(mktemp)
  set +e
  "$REA_BIN" --no-cache --vm-trace-head=3 "$fixture" >"$tmp_out" 2>"$tmp_err"
  status=$?
  set -e
  if [ $status -ne 0 ]; then
    echo "rea --vm-trace-head exited with $status" >&2
    cat "$tmp_err" >&2
    EXIT_CODE=1
  elif ! grep -q "\[VM-TRACE\]" "$tmp_err"; then
    echo "rea --vm-trace-head did not emit trace output" >&2
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

}

# Initialize array of tests to skip. When REA_SKIP_TESTS is unset or empty,
# avoid "unbound variable" errors under `set -u` by explicitly declaring an
# empty array. Otherwise, split the space-separated environment variable into
# an array.
SKIP_TESTS=(foo)
if [[ -n "${REA_SKIP_TESTS:-}" ]]; then
  IFS=' ' read -r -a SKIP_TESTS <<< "$REA_SKIP_TESTS"
fi

should_skip() {
  local t="$1"
  for s in "${SKIP_TESTS[@]}"; do
    if [[ "$s" == "$t" ]]; then
      return 0
    fi
  done
  return 1
}

if [ ! -x "$REA_BIN" ]; then
  echo "rea binary not found at $REA_BIN" >&2
  exit 1
fi

# Use an isolated HOME to avoid interference from existing caches.
TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"
trap 'rm -rf "$TEST_HOME"' EXIT

cd "$ROOT_DIR"
EXIT_CODE=0

check_ext_builtin_dump "$REA_BIN" rea
exercise_rea_cli_smoke

if has_ext_builtin_category "$REA_BIN" sqlite; then
  REA_SQLITE_AVAILABLE=1
else
  REA_SQLITE_AVAILABLE=0
fi

if has_ext_builtin_category "$REA_BIN" 3d; then
  REA_THREED_AVAILABLE=1
else
  REA_THREED_AVAILABLE=0
fi

for src in "$SCRIPT_DIR"/rea/*.rea; do
  test_name=$(basename "$src" .rea)

  if should_skip "$test_name"; then
    echo "---- $test_name (skipped) ----"
    continue
  fi
  if [ -f "$SCRIPT_DIR/rea/$test_name.sqlite" ] && [ "$REA_SQLITE_AVAILABLE" -ne 1 ]; then
    echo "---- $test_name (skipped: SQLite builtins disabled) ----"
    continue
  fi
  if [ "$REA_THREED_AVAILABLE" -ne 1 ] && { [ "$test_name" = "balls3d_builtin_compare" ] || [ "$test_name" = "balls3d_demo_regression" ]; }; then
    echo "---- $test_name (skipped: 3D builtins disabled) ----"
    continue
  fi
  in_file="$SCRIPT_DIR/rea/$test_name.in"
  out_file="$SCRIPT_DIR/rea/$test_name.out"
  err_file="$SCRIPT_DIR/rea/$test_name.err"
  src_rel=${src#$ROOT_DIR/}
  disasm_file="$SCRIPT_DIR/rea/$test_name.disasm"
  disasm_stdout=""
  disasm_stderr=""

  if [ -f "$disasm_file" ]; then
    disasm_stdout=$(mktemp)
    disasm_stderr=$(mktemp)
    set +e
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$REA_BIN" --dump-bytecode-only "$src_rel" \
      > "$disasm_stdout" 2> "$disasm_stderr"
    disasm_status=$?
    set -e
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$disasm_stderr" > "$disasm_stderr.clean"
    mv "$disasm_stderr.clean" "$disasm_stderr"
    perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$disasm_stderr" > "$disasm_stderr.clean"
    mv "$disasm_stderr.clean" "$disasm_stderr"
    rel_src="Tests/rea/$test_name.rea"
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
  perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$actual_err" > "$actual_err.clean"
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

  if [ -n "$disasm_stdout" ]; then rm -f "$disasm_stdout"; fi
  if [ -n "$disasm_stderr" ]; then rm -f "$disasm_stderr"; fi
  rm -f "$actual_out" "$actual_err"
  echo
  echo
done

# Ensure the Hangman example compiles cleanly and that the compiler emits
# the WordRepository vtable before the global HangmanGame constructor runs.
echo "---- HangmanExample ----"
hangman_disasm=$(mktemp)
set +e
python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$REA_BIN" --no-cache --dump-bytecode-only Examples/rea/base/hangman5 \
  > /dev/null 2> "$hangman_disasm"
status=$?
set -e
perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$hangman_disasm" > "$hangman_disasm.clean"
mv "$hangman_disasm.clean" "$hangman_disasm"
perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$hangman_disasm" > "$hangman_disasm.clean"
mv "$hangman_disasm.clean" "$hangman_disasm"
if [ $status -ne 0 ]; then
  echo "Hangman example failed to compile" >&2
  cat "$hangman_disasm"
  EXIT_CODE=1
else
  if ! python3 - "$hangman_disasm" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text().splitlines()
def_line = call_line = None
for idx, line in enumerate(text, 1):
    if def_line is None and "DEFINE_GLOBAL" in line and "'wordrepository_vtable'" in line:
        def_line = idx
    if call_line is None and "CALL_USER_PROC" in line and "'hangmangame'" in line:
        call_line = idx
if def_line is None:
    print("Hangman example missing wordrepository vtable definition", file=sys.stderr)
    sys.exit(1)
if call_line is None:
    print("Hangman example missing hangmangame call", file=sys.stderr)
    sys.exit(1)
if def_line > call_line:
    print(
        f"wordrepository vtable defined after hangmangame call (def_line={def_line}, call_line={call_line})",
        file=sys.stderr,
    )
    sys.exit(1)
PY
  then
    EXIT_CODE=1
  fi
fi
rm -f "$hangman_disasm"
echo
echo

# Basic cache reuse test for the Rea front end
echo "---- CacheReuseTest ----"
tmp_home=$(mktemp -d)
src_dir=$(mktemp -d)
cat > "$src_dir/CacheTest.rea" <<'EOF'
writeln("first");
EOF
shift_mtime "$src_dir/CacheTest.rea" -5
set +e
(cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" CacheTest.rea > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  set +e
  (cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" CacheTest.rea > "$tmp_home/out2" 2> "$tmp_home/err2")
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

# Cache invalidation test when the Rea binary is newer than the cache
echo "---- CacheBinaryStalenessTest ----"
tmp_home=$(mktemp -d)
src_dir=$(mktemp -d)
cat > "$src_dir/BinaryTest.rea" <<'EOF'
writeln("first");
EOF
shift_mtime "$src_dir/BinaryTest.rea" -5
set +e
(cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" BinaryTest.rea > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  shift_mtime "$REA_BIN" 5
  set +e
  (cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" BinaryTest.rea > "$tmp_home/out2" 2> "$tmp_home/err2")
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
