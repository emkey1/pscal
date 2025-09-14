#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd -P)"
REA_BIN="$ROOT_DIR/build/bin/rea"
RUNNER_PY="$ROOT_DIR/Tests/tools/run_with_timeout.py"
TEST_TIMEOUT="${TEST_TIMEOUT:-25}"

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

for src in "$SCRIPT_DIR"/rea/*.rea; do
  test_name=$(basename "$src" .rea)

  if should_skip "$test_name"; then
    echo "---- $test_name (skipped) ----"
    continue
  fi
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

  rm -f "$actual_out" "$actual_err"
  echo
  echo
 done

# Basic cache reuse test for the Rea front end
echo "---- CacheReuseTest ----"
tmp_home=$(mktemp -d)
src_dir=$(mktemp -d)
cat > "$src_dir/CacheTest.rea" <<'EOF'
writeln("first");
EOF
sleep 1
set +e
(cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" CacheTest.rea > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  sleep 2
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
sleep 1
set +e
(cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" BinaryTest.rea > "$tmp_home/out1" 2> "$tmp_home/err1")
status1=$?
set -e
if [ $status1 -eq 0 ] && grep -q 'first' "$tmp_home/out1"; then
  sleep 2
  touch "$REA_BIN"
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
