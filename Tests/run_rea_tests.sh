#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(os.path.dirname(sys.argv[1])))' "${BASH_SOURCE[0]}")"
ROOT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$SCRIPT_DIR/..")"
REA_BIN="$ROOT_DIR/build/bin/rea"
RUNNER_PY="$ROOT_DIR/Tests/tools/run_with_timeout.py"
TEST_TIMEOUT="${TEST_TIMEOUT:-25}"

. "$SCRIPT_DIR/tools/harness_utils.sh"
harness_init

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

strip_ansi_inplace() {
    local path="$1"
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

normalise_rea_stdout() {
    strip_ansi_inplace "$1"
}

normalise_rea_stderr() {
    strip_ansi_inplace "$1"
    perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached bytecode.*\n//m' "$1" > "$1.clean"
    mv "$1.clean" "$1"
}

has_ext_builtin_category() {
    local binary="$1"
    local category="$2"
    set +e
    "$binary" --dump-ext-builtins | grep -Ei "^category[[:space:]]+${category}$" >/dev/null
    local status=$?
    set -e
    return $status
}

run_rea_ext_builtin_dump() {
    local label="$1"
    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$REA_BIN" --dump-ext-builtins >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    if [ $status -ne 0 ]; then
        printf 'rea --dump-ext-builtins exited with %d for %s\n' "$status" "$label"
        if [ -s "$tmp_err" ]; then
            printf 'stderr:\n%s\n' "$(cat "$tmp_err")"
        fi
        rm -f "$tmp_out" "$tmp_err"
        return 1
    fi

    if [ -s "$tmp_err" ]; then
        printf 'rea --dump-ext-builtins produced stderr for %s:\n%s\n' "$label" "$(cat "$tmp_err")"
        rm -f "$tmp_out" "$tmp_err"
        return 1
    fi

    if ! python3 - "$tmp_out" <<'PY'
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
        printf 'rea --dump-ext-builtins emitted unexpected directives for %s\n' "$label"
        printf '%s\n' "$(cat "$tmp_out")"
        rm -f "$tmp_out" "$tmp_err"
        return 1
    fi

    rm -f "$tmp_out" "$tmp_err"
    return 0
}

rea_cli_version() {
    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$REA_BIN" -v >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("rea -v exited with $status")
    fi
    if [ -s "$tmp_err" ]; then
        issues+=("rea -v wrote to stderr:\n$(cat "$tmp_err")")
    fi
    if ! grep -q "latest tag:" "$tmp_out"; then
        issues+=("rea -v stdout missing 'latest tag:' line:\n$(cat "$tmp_out")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

rea_cli_strict_dump() {
    local fixture="$SCRIPT_DIR/tools/fixtures/cli_rea.rea"
    if [ ! -f "$fixture" ]; then
        printf 'Rea CLI fixture missing at %s\n' "$fixture"
        return 1
    fi

    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$REA_BIN" --no-cache --strict --dump-bytecode-only "$fixture" >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("rea --strict --dump-bytecode-only exited with $status")
    fi
    if ! grep -q "Compiling Main Program AST to Bytecode" "$tmp_err"; then
        issues+=("stderr missing disassembly banner:\n$(cat "$tmp_err")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

rea_cli_no_run() {
    local fixture="$SCRIPT_DIR/tools/fixtures/cli_rea.rea"
    if [ ! -f "$fixture" ]; then
        printf 'Rea CLI fixture missing at %s\n' "$fixture"
        return 1
    fi

    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$REA_BIN" --verbose --no-cache --no-run "$fixture" >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("rea --no-run exited with $status")
    fi
    if [ -s "$tmp_out" ]; then
        issues+=("rea --no-run produced stdout:\n$(cat "$tmp_out")")
    fi
    if ! grep -q "Compilation successful" "$tmp_err"; then
        issues+=("stderr missing compilation banner:\n$(cat "$tmp_err")")
    fi
    if grep -q -- "--- executing Program" "$tmp_err"; then
        issues+=("stderr should not announce VM execution:\n$(cat "$tmp_err")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

rea_cli_vm_trace() {
    local fixture="$SCRIPT_DIR/tools/fixtures/cli_rea.rea"
    if [ ! -f "$fixture" ]; then
        printf 'Rea CLI fixture missing at %s\n' "$fixture"
        return 1
    fi

    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$REA_BIN" --no-cache --vm-trace-head=3 "$fixture" >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("rea --vm-trace-head exited with $status")
    fi
    if ! grep -q "[VM-TRACE]" "$tmp_err"; then
        issues+=("stderr missing [VM-TRACE] entries:\n$(cat "$tmp_err")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

SKIP_TESTS=()
if [ -n "${REA_SKIP_TESTS:-}" ]; then
    IFS=' ' read -r -a SKIP_TESTS <<< "$REA_SKIP_TESTS"
fi

should_skip() {
    local candidate="$1"
    local entry
    local found=1

    set +u
    for entry in "${SKIP_TESTS[@]}"; do
        if [ "$entry" = "$candidate" ]; then
            found=0
            break
        fi
    done
    set -u

    if [ $found -eq 0 ]; then
        return 0
    fi
    return 1
}

run_rea_fixture() {
    local test_name="$1"
    local src="$SCRIPT_DIR/rea/$test_name.rea"
    local src_rel="${src#$ROOT_DIR/}"
    local input_file="$SCRIPT_DIR/rea/$test_name.in"
    local stdout_expect="$SCRIPT_DIR/rea/$test_name.out"
    local stderr_expect="$SCRIPT_DIR/rea/$test_name.err"
    local disasm_expect="$SCRIPT_DIR/rea/$test_name.disasm"
    local sqlite_marker="$SCRIPT_DIR/rea/$test_name.sqlite"
    local args_file="$SCRIPT_DIR/rea/$test_name.args"

    if should_skip "$test_name"; then
        harness_report SKIP "rea_${test_name}" "Rea fixture $test_name" "Skipped via REA_SKIP_TESTS"
        return
    fi

    if [ -f "$sqlite_marker" ] && [ "$REA_SQLITE_AVAILABLE" -ne 1 ]; then
        harness_report SKIP "rea_${test_name}" "Rea fixture $test_name" "SQLite builtins disabled"
        return
    fi

    if [ "$REA_THREED_AVAILABLE" -ne 1 ] && { [ "$test_name" = "balls3d_builtin_compare" ] || [ "$test_name" = "balls3d_demo_regression" ]; }; then
        harness_report SKIP "rea_${test_name}" "Rea fixture $test_name" "3D builtins disabled"
        return
    fi

    local arg_list=()
    local args_source=""
    local args_file_present=0
    if [ -f "$args_file" ]; then
        args_file_present=1
        # Read the first line verbatim; empty files intentionally signal
        # "no extra arguments", so treat a failed read as an empty string.
        if IFS= read -r args_source < "$args_file"; then
            :
        else
            args_source=""
        fi
    fi

    if [ -n "$args_source" ]; then
        read -r -a arg_list <<< "$args_source"
    elif [ $args_file_present -eq 0 ]; then
        arg_list=(--dump-bytecode-only)
    fi

    local details=()
    local status="PASS"

    if [ -f "$disasm_expect" ]; then
        local disasm_stderr=$(mktemp)
        set +e
        python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$REA_BIN" --dump-bytecode-only "$src_rel" > /dev/null 2> "$disasm_stderr"
        local disasm_status=$?
        set -e
        strip_ansi_inplace "$disasm_stderr"
        perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached bytecode.*\n//m' "$disasm_stderr" > "$disasm_stderr.clean"
        mv "$disasm_stderr.clean" "$disasm_stderr"
        rel_src="Tests/rea/$test_name.rea"
        sed -i.bak "s|$src|$rel_src|" "$disasm_stderr" 2>/dev/null || true
        rm -f "$disasm_stderr.bak"
        if [ $disasm_status -ne 0 ]; then
            status="FAIL"
            details+=("Disassembly exited with status $disasm_status")
            if [ -s "$disasm_stderr" ]; then
                details+=("Disassembly stderr:\n$(cat "$disasm_stderr")")
            fi
        else
            set +e
            diff_output=$(diff -u "$disasm_expect" "$disasm_stderr")
            local diff_status=$?
            set -e
            if [ $diff_status -ne 0 ]; then
                status="FAIL"
                if [ $diff_status -eq 1 ]; then
                    details+=("Disassembly mismatch:\n$diff_output")
                else
                    details+=("Disassembly diff failed with status $diff_status")
                fi
            fi
        fi
        rm -f "$disasm_stderr"
    fi

    local actual_out=$(mktemp)
    local actual_err=$(mktemp)

    if [ "$status" = "PASS" ]; then
        local -a cmd=(python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$REA_BIN")
        if (( ${#arg_list[@]} )); then
            cmd+=("${arg_list[@]}")
        fi
        cmd+=("$src_rel")

        set +e
        if [ -f "$input_file" ]; then
            "${cmd[@]}" < "$input_file" > "$actual_out" 2> "$actual_err"
        else
            "${cmd[@]}" > "$actual_out" 2> "$actual_err"
        fi
        local run_status=$?
        set -e

        normalise_rea_stdout "$actual_out"
        normalise_rea_stderr "$actual_err"

        if [ -f "$stdout_expect" ]; then
            set +e
            diff_output=$(diff -u "$stdout_expect" "$actual_out")
            local diff_status=$?
            set -e
            if [ $diff_status -ne 0 ]; then
                status="FAIL"
                if [ $diff_status -eq 1 ]; then
                    details+=("stdout mismatch:\n$diff_output")
                else
                    details+=("diff on stdout failed with status $diff_status")
                fi
            fi
        else
            if [ -s "$actual_out" ]; then
                status="FAIL"
                details+=("Unexpected stdout:\n$(cat "$actual_out")")
            fi
        fi

        if [ -f "$stderr_expect" ]; then
            set +e
            diff_output=$(diff -u "$stderr_expect" "$actual_err")
            local diff_status=$?
            set -e
            if [ $diff_status -ne 0 ]; then
                status="FAIL"
                if [ $diff_status -eq 1 ]; then
                    details+=("stderr mismatch:\n$diff_output")
                else
                    details+=("diff on stderr failed with status $diff_status")
                fi
            fi
        else
            if [ -s "$actual_err" ]; then
                status="FAIL"
                details+=("Unexpected stderr:\n$(cat "$actual_err")")
            fi
        fi

        if [ $run_status -ne 0 ]; then
            status="FAIL"
            details+=("Rea invocation exited with $run_status")
        fi
    fi

    rm -f "$actual_out" "$actual_err"

    if [ "$status" = "PASS" ]; then
        harness_report PASS "rea_${test_name}" "Rea fixture $test_name"
    else
        harness_report FAIL "rea_${test_name}" "Rea fixture $test_name" "${details[@]}"
    fi
}

rea_hangman_example() {
    local disasm=$(mktemp)
    set +e
    python3 "$RUNNER_PY" --timeout "$TEST_TIMEOUT" "$REA_BIN" --no-cache --dump-bytecode-only Examples/rea/base/hangman5 > /dev/null 2> "$disasm"
    local status=$?
    set -e

    normalise_rea_stderr "$disasm"

    if [ $status -ne 0 ]; then
        printf 'Hangman example failed to compile\n'
        printf '%s\n' "$(cat "$disasm")"
        rm -f "$disasm"
        return 1
    fi

    if python3 - "$disasm" <<'PY'
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
        rm -f "$disasm"
        return 0
    fi

    rm -f "$disasm"
    return 1
}

rea_cache_reuse_test() {
    local tmp_home src_dir
    tmp_home=$(mktemp -d)
    src_dir=$(mktemp -d)
    cat > "$src_dir/CacheTest.rea" <<'EOF'
writeln("first");
EOF
    shift_mtime "$src_dir/CacheTest.rea" -5

    set +e
    (cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" --verbose CacheTest.rea > "$tmp_home/out1" 2> "$tmp_home/err1")
    local status1=$?
    set -e

    local issues=()
    if [ $status1 -ne 0 ]; then
        issues+=("Initial compile exited with $status1")
    elif ! grep -q 'first' "$tmp_home/out1"; then
        issues+=("Initial compile missing expected stdout")
    else
        set +e
        (cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" --verbose CacheTest.rea > "$tmp_home/out2" 2> "$tmp_home/err2")
        local status2=$?
        set -e
        if [ $status2 -ne 0 ]; then
            issues+=("Cached compile exited with $status2")
        elif ! grep -q 'Loaded cached bytecode' "$tmp_home/err2"; then
            issues+=("Expected cached bytecode notice missing")
        fi
    fi

    rm -rf "$tmp_home" "$src_dir"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

rea_cache_binary_staleness_test() {
    local tmp_home src_dir
    tmp_home=$(mktemp -d)
    src_dir=$(mktemp -d)
    cat > "$src_dir/BinaryTest.rea" <<'EOF'
writeln("first");
EOF
    shift_mtime "$src_dir/BinaryTest.rea" -5

    set +e
    (cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" --verbose BinaryTest.rea > "$tmp_home/out1" 2> "$tmp_home/err1")
    local status1=$?
    set -e

    local issues=()
    if [ $status1 -ne 0 ]; then
        issues+=("Initial compile exited with $status1")
    elif ! grep -q 'first' "$tmp_home/out1"; then
        issues+=("Initial compile missing expected stdout")
    else
        shift_mtime "$REA_BIN" 5
        set +e
        (cd "$src_dir" && HOME="$tmp_home" "$REA_BIN" --verbose BinaryTest.rea > "$tmp_home/out2" 2> "$tmp_home/err2")
        local status2=$?
        set -e
        if [ $status2 -ne 0 ]; then
            issues+=("Recompile after binary touch exited with $status2")
        elif grep -q 'Loaded cached bytecode' "$tmp_home/err2"; then
            issues+=("Cache should have been invalidated after binary change")
        fi
    fi

    rm -rf "$tmp_home" "$src_dir"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

if [ ! -x "$REA_BIN" ]; then
    echo "rea binary not found at $REA_BIN" >&2
    exit 1
fi

TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"
trap 'rm -rf "$TEST_HOME"' EXIT

cd "$ROOT_DIR"

if details=$(run_rea_ext_builtin_dump rea); then
    harness_report PASS "rea_ext_builtin_dump" "rea --dump-ext-builtins validates structure"
else
    harness_report FAIL "rea_ext_builtin_dump" "rea --dump-ext-builtins validates structure" "$details"
fi

if details=$(rea_cli_version); then
    harness_report PASS "rea_cli_version" "rea -v reports latest tag"
else
    harness_report FAIL "rea_cli_version" "rea -v reports latest tag" "$details"
fi

if details=$(rea_cli_strict_dump); then
    harness_report PASS "rea_cli_strict_dump" "rea --strict --dump-bytecode-only prints banner"
else
    harness_report FAIL "rea_cli_strict_dump" "rea --strict --dump-bytecode-only prints banner" "$details"
fi

if details=$(rea_cli_no_run); then
    harness_report PASS "rea_cli_no_run" "rea --no-run compiles without executing"
else
    harness_report FAIL "rea_cli_no_run" "rea --no-run compiles without executing" "$details"
fi

if details=$(rea_cli_vm_trace); then
    harness_report PASS "rea_cli_vm_trace" "rea --vm-trace-head produces trace"
else
    harness_report FAIL "rea_cli_vm_trace" "rea --vm-trace-head produces trace" "$details"
fi

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

shopt -s nullglob
for src in "$SCRIPT_DIR"/rea/*.rea; do
    test_name=$(basename "$src" .rea)
    run_rea_fixture "$test_name"
done
shopt -u nullglob

if details=$(rea_hangman_example); then
    harness_report PASS "rea_hangman_example" "Hangman example emits vtable before constructor"
else
    harness_report FAIL "rea_hangman_example" "Hangman example emits vtable before constructor" "$details"
fi

if details=$(rea_cache_reuse_test); then
    harness_report PASS "rea_cache_reuse" "Cache reuse surfaces bytecode reuse notice"
else
    harness_report FAIL "rea_cache_reuse" "Cache reuse surfaces bytecode reuse notice" "$details"
fi

if details=$(rea_cache_binary_staleness_test); then
    harness_report PASS "rea_cache_binary_staleness" "Binary timestamp invalidates cached bytecode"
else
    harness_report FAIL "rea_cache_binary_staleness" "Binary timestamp invalidates cached bytecode" "$details"
fi

harness_summary "Rea"
if harness_exit_code; then
    exit 0
fi
exit 1
