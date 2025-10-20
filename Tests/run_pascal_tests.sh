#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PASCAL_BIN="$ROOT_DIR/build/bin/pascal"
PASCAL_ARGS=(--no-cache)

# Shared result formatting helpers
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

has_ext_builtin_category() {
    local binary="$1"
    local category="$2"
    set +e
    "$binary" --dump-ext-builtins | grep -Ei "^category[[:space:]]+${category}$" >/dev/null
    local status=$?
    set -e
    return $status
}

strip_ansi_inplace() {
    local path="$1"
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]//g; s/\e\][^\a]*(?:\a|\e\\)//g' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

normalise_pascal_stdout() {
    local path="$1"
    strip_ansi_inplace "$path"
    perl -pe 's/pid=[0-9]+/pid=<PID>/g' "$path" > "$path.clean"
    mv "$path.clean" "$path"
    perl -ne 'print unless /^[12][0-9]{3}-[01][0-9]-[0-3][0-9]/' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

normalise_pascal_stderr() {
    local path="$1"
    strip_ansi_inplace "$path"
    perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached bytecode.*\n//m' "$path" > "$path.clean"
    mv "$path.clean" "$path"
    perl -ne 'print unless /Warning: user-defined .* overrides builtin/' "$path" > "$path.clean"
    mv "$path.clean" "$path"
    perl -ne 'print unless /Compiler warning: assigning .* may lose precision/' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

run_pascal_ext_builtin_dump() {
    local label="$1"
    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$PASCAL_BIN" --dump-ext-builtins >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    if [ $status -ne 0 ]; then
        printf 'pascal --dump-ext-builtins exited with %d for %s\n' "$status" "$label"
        if [ -s "$tmp_err" ]; then
            printf 'stderr:\n%s\n' "$(cat "$tmp_err")"
        fi
        rm -f "$tmp_out" "$tmp_err"
        return 1
    fi

    if [ -s "$tmp_err" ]; then
        printf 'pascal --dump-ext-builtins produced stderr for %s:\n%s\n' "$label" "$(cat "$tmp_err")"
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
        printf 'pascal --dump-ext-builtins emitted unexpected directives for %s\n' "$label"
        printf '%s\n' "$(cat "$tmp_out")"
        rm -f "$tmp_out" "$tmp_err"
        return 1
    fi

    rm -f "$tmp_out" "$tmp_err"
    return 0
}

pascal_cli_version() {
    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$PASCAL_BIN" -v >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("pascal -v exited with $status")
    fi
    if [ -s "$tmp_err" ]; then
        issues+=("pascal -v wrote to stderr:\n$(cat "$tmp_err")")
    fi
    if ! grep -q "latest tag:" "$tmp_out"; then
        issues+=("pascal -v stdout missing 'latest tag:' line:\n$(cat "$tmp_out")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

pascal_cli_dump_ast_json() {
    local fixture="$SCRIPT_DIR/tools/fixtures/cli_pascal.pas"
    if [ ! -f "$fixture" ]; then
        printf 'Pascal CLI fixture missing at %s\n' "$fixture"
        return 1
    fi

    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$PASCAL_BIN" --no-cache --dump-ast-json "$fixture" >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("pascal --dump-ast-json exited with $status")
    fi
    if ! grep -q "Dumping AST" "$tmp_err"; then
        issues+=("stderr missing 'Dumping AST' banner:\n$(cat "$tmp_err")")
    fi
    if ! grep -q '"type"' "$tmp_out"; then
        issues+=("stdout missing type entries:\n$(cat "$tmp_out")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

pascal_cli_vm_trace() {
    local fixture="$SCRIPT_DIR/tools/fixtures/cli_pascal.pas"
    if [ ! -f "$fixture" ]; then
        printf 'Pascal CLI fixture missing at %s\n' "$fixture"
        return 1
    fi

    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$PASCAL_BIN" --no-cache --vm-trace-head=3 "$fixture" >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("pascal --vm-trace-head exited with $status")
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

run_pascal_fixture() {
    local test_name="$1"
    local input_file="$SCRIPT_DIR/Pascal/$test_name.in"
    local stdout_expect="$SCRIPT_DIR/Pascal/$test_name.out"
    local stderr_expect="$SCRIPT_DIR/Pascal/$test_name.err"
    local disasm_expect="$SCRIPT_DIR/Pascal/$test_name.disasm"
    local sqlite_marker="$SCRIPT_DIR/Pascal/$test_name.sqlite"
    local net_marker="$SCRIPT_DIR/Pascal/$test_name.net"

    local details=()
    local status="PASS"

    if [ "$test_name" = "SDLFeaturesTest" ] || [ "$test_name" = "GetScreenSizeTest" ]; then
        if [ "$PASCAL_GRAPHICS_AVAILABLE" -ne 1 ]; then
            harness_report SKIP "pascal_${test_name}" "Pascal fixture $test_name" "Graphics builtins unavailable"
            return
        fi
        if [ "${RUN_SDL:-0}" != "1" ] && { [ "$SDL_ENABLED" -eq 0 ] || [ "${SDL_VIDEODRIVER:-}" = "dummy" ]; }; then
            harness_report SKIP "pascal_${test_name}" "Pascal fixture $test_name" "SDL disabled"
            return
        fi
    fi

    if [ -f "$sqlite_marker" ] && [ "$PASCAL_SQLITE_AVAILABLE" -ne 1 ]; then
        harness_report SKIP "pascal_${test_name}" "Pascal fixture $test_name" "SQLite builtins disabled"
        return
    fi

    if [ -f "$net_marker" ] && [ "${RUN_NET_TESTS:-0}" != "1" ]; then
        harness_report SKIP "pascal_${test_name}" "Pascal fixture $test_name" "Network tests disabled (set RUN_NET_TESTS=1)"
        return
    fi

    local actual_out actual_err disasm_stderr
    actual_out=$(mktemp)
    actual_err=$(mktemp)

    if [ -f "$disasm_expect" ]; then
        disasm_stderr=$(mktemp)
        set +e
        (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "${PASCAL_ARGS[@]}" --dump-bytecode-only "Pascal/$test_name") \
            > /dev/null 2> "$disasm_stderr"
        local disasm_status=$?
        set -e
        strip_ansi_inplace "$disasm_stderr"
        perl -ne 'print unless /^Loaded cached bytecode/' "$disasm_stderr" > "$disasm_stderr.clean"
        mv "$disasm_stderr.clean" "$disasm_stderr"
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

    if [ "$status" = "PASS" ]; then
        set +e
        if [ "$test_name" = "SDLFeaturesTest" ]; then
            (cd "$SCRIPT_DIR" && printf 'Q\n' | "$PASCAL_BIN" "${PASCAL_ARGS[@]}" "Pascal/$test_name" > "$actual_out" 2> "$actual_err")
        elif [ -f "$input_file" ]; then
            (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "${PASCAL_ARGS[@]}" "Pascal/$test_name" < "$input_file" > "$actual_out" 2> "$actual_err")
        else
            (cd "$SCRIPT_DIR" && "$PASCAL_BIN" "${PASCAL_ARGS[@]}" "Pascal/$test_name" > "$actual_out" 2> "$actual_err")
        fi
        local run_status=$?
        set -e

        normalise_pascal_stdout "$actual_out"
        normalise_pascal_stderr "$actual_err"
        if [ ! -f "$stderr_expect" ]; then
            head -n 2 "$actual_err" > "$actual_err.trim"
            mv "$actual_err.trim" "$actual_err"
        fi

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
            if [ $run_status -eq 0 ] && [ "$status" = "PASS" ]; then
                status="FAIL"
                details+=("Expected non-zero exit but got $run_status")
            fi
        else
            if [ $run_status -ne 0 ]; then
                status="FAIL"
                details+=("pscal run exited with $run_status")
                if [ -s "$actual_err" ]; then
                    details+=("stderr:\n$(cat "$actual_err")")
                fi
            elif [ -s "$actual_err" ]; then
                status="FAIL"
                details+=("Unexpected stderr:\n$(cat "$actual_err")")
            fi
        fi
    fi

    rm -f "$actual_out" "$actual_err"

    if [ "$status" = "PASS" ]; then
        harness_report PASS "pascal_${test_name}" "Pascal fixture $test_name"
    else
        harness_report FAIL "pascal_${test_name}" "Pascal fixture $test_name" "${details[@]}"
    fi
}

run_cache_staleness_test() {
    local tmp_home src_dir src_file
    tmp_home=$(mktemp -d)
    src_dir=$(mktemp -d)
    src_file="$src_dir/CacheStalenessTest"

    cat > "$src_file" <<'EOF'
program CacheStalenessTest;
begin
  writeln('first');
end.
EOF

    shift_mtime "$src_file" -5

    set +e
    (cd "$src_dir" && HOME="$tmp_home" "$PASCAL_BIN" "CacheStalenessTest" > "$tmp_home/out1" 2> "$tmp_home/err1")
    local status1=$?
    set -e

    if [ $status1 -ne 0 ]; then
        printf 'Initial run exited with %d\n' "$status1"
        printf 'stderr:\n%s\n' "$(cat "$tmp_home/err1")"
        rm -rf "$tmp_home" "$src_dir"
        return 1
    fi

    if ! grep -q 'first' "$tmp_home/out1"; then
        printf 'Initial run output missing expected text\n'
        rm -rf "$tmp_home" "$src_dir"
        return 1
    fi

    cat > "$src_file" <<'EOF'
program CacheStalenessTest;
begin
  writeln('second');
end.
EOF

    shift_mtime "$src_file" 5

    set +e
    (cd "$src_dir" && HOME="$tmp_home" "$PASCAL_BIN" "CacheStalenessTest" > "$tmp_home/out2" 2> "$tmp_home/err2")
    local status2=$?
    set -e

    local issues=()

    if [ $status2 -ne 0 ]; then
        issues+=("Second run exited with $status2")
    fi

    if ! grep -q 'second' "$tmp_home/out2"; then
        issues+=("Cache invalidation failed; stdout:\n$(cat "$tmp_home/out2")")
    fi

    rm -rf "$tmp_home" "$src_dir"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

if [ ! -x "$PASCAL_BIN" ]; then
    echo "pascal binary not found at $PASCAL_BIN" >&2
    exit 1
fi

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

if [ "${RUN_SDL:-0}" != "1" ] && [ "$SDL_ENABLED" -eq 1 ]; then
    export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
    export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
fi

if has_ext_builtin_category "$PASCAL_BIN" sqlite; then
    PASCAL_SQLITE_AVAILABLE=1
else
    PASCAL_SQLITE_AVAILABLE=0
fi

if has_ext_builtin_category "$PASCAL_BIN" graphics; then
    PASCAL_GRAPHICS_AVAILABLE=1
else
    PASCAL_GRAPHICS_AVAILABLE=0
fi

if details=$(run_pascal_ext_builtin_dump pascal); then
    harness_report PASS "pascal_ext_builtin_dump" "pascal --dump-ext-builtins validates structure"
else
    harness_report FAIL "pascal_ext_builtin_dump" "pascal --dump-ext-builtins validates structure" "$details"
fi

if details=$(pascal_cli_version); then
    harness_report PASS "pascal_cli_version" "pascal -v reports latest tag"
else
    harness_report FAIL "pascal_cli_version" "pascal -v reports latest tag" "$details"
fi

if details=$(pascal_cli_dump_ast_json); then
    harness_report PASS "pascal_cli_dump_ast_json" "pascal --dump-ast-json emits JSON"
else
    harness_report FAIL "pascal_cli_dump_ast_json" "pascal --dump-ast-json emits JSON" "$details"
fi

if details=$(pascal_cli_vm_trace); then
    harness_report PASS "pascal_cli_vm_trace" "pascal --vm-trace-head produces trace"
else
    harness_report FAIL "pascal_cli_vm_trace" "pascal --vm-trace-head produces trace" "$details"
fi

shopt -s nullglob
for src in "$SCRIPT_DIR"/Pascal/*; do
    test_name=$(basename "$src")
    if [[ "$test_name" == *.* ]]; then
        continue
    fi
    run_pascal_fixture "$test_name"
done
shopt -u nullglob

if details=$(run_cache_staleness_test); then
    harness_report PASS "pascal_cache_staleness" "Cache invalidation honours matching timestamps"
else
    harness_report FAIL "pascal_cache_staleness" "Cache invalidation honours matching timestamps" "$details"
fi

harness_summary "Pascal"
if harness_exit_code; then
    exit 0
fi
exit 1
