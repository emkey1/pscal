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

. "$SCRIPT_DIR/tools/harness_utils.sh"
harness_init

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

strip_ansi_inplace() {
    local path="$1"
    perl -pe 's/\e\[[0-9;?]*[ -\/]*[@-~]|\e\][^\a]*(?:\a|\e\\)//g' "$path" > "$path.clean"
    mv "$path.clean" "$path"
}

normalise_clike_stdout() {
    strip_ansi_inplace "$1"
}

normalise_clike_stderr() {
    strip_ansi_inplace "$1"
    perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$1" > "$1.clean"
    mv "$1.clean" "$1"
    head -n 2 "$1" > "$1.trim"
    mv "$1.trim" "$1"
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

run_clike_ext_builtin_dump() {
    local label="$1"
    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$CLIKE_BIN" --dump-ext-builtins >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    if [ $status -ne 0 ]; then
        printf 'clike --dump-ext-builtins exited with %d for %s\n' "$status" "$label"
        if [ -s "$tmp_err" ]; then
            printf 'stderr:\n%s\n' "$(cat "$tmp_err")"
        fi
        rm -f "$tmp_out" "$tmp_err"
        return 1
    fi

    if [ -s "$tmp_err" ]; then
        printf 'clike --dump-ext-builtins produced stderr for %s:\n%s\n' "$label" "$(cat "$tmp_err")"
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
        printf 'clike --dump-ext-builtins emitted unexpected directives for %s\n' "$label"
        printf '%s\n' "$(cat "$tmp_out")"
        rm -f "$tmp_out" "$tmp_err"
        return 1
    fi

    rm -f "$tmp_out" "$tmp_err"
    return 0
}

clike_cli_version() {
    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$CLIKE_BIN" -v >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("clike -v exited with $status")
    fi
    if [ -s "$tmp_err" ]; then
        issues+=("clike -v wrote to stderr:\n$(cat "$tmp_err")")
    fi
    if ! grep -q "latest tag:" "$tmp_out"; then
        issues+=("clike -v stdout missing 'latest tag:' line:\n$(cat "$tmp_out")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

clike_cli_dump_ast_json() {
    local fixture="$SCRIPT_DIR/tools/fixtures/cli_clike.cl"
    if [ ! -f "$fixture" ]; then
        printf 'Clike CLI fixture missing at %s\n' "$fixture"
        return 1
    fi

    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$CLIKE_BIN" --no-cache --dump-ast-json "$fixture" >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("clike --dump-ast-json exited with $status")
    fi
    if ! grep -q "Dumping AST" "$tmp_err"; then
        issues+=("stderr missing 'Dumping AST' banner:\n$(cat "$tmp_err")")
    fi
    if ! grep -q '"node_type"' "$tmp_out"; then
        issues+=("stdout missing node_type entries:\n$(cat "$tmp_out")")
    fi

    rm -f "$tmp_out" "$tmp_err"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

clike_cli_vm_trace() {
    local fixture="$SCRIPT_DIR/tools/fixtures/cli_clike.cl"
    if [ ! -f "$fixture" ]; then
        printf 'Clike CLI fixture missing at %s\n' "$fixture"
        return 1
    fi

    local tmp_out tmp_err
    tmp_out=$(mktemp)
    tmp_err=$(mktemp)

    set +e
    "$CLIKE_BIN" --no-cache --vm-trace-head=3 "$fixture" >"$tmp_out" 2>"$tmp_err"
    local status=$?
    set -e

    local issues=()
    if [ $status -ne 0 ]; then
        issues+=("clike --vm-trace-head exited with $status")
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

run_clike_fixture() {
    local test_name="$1"
    local src="$SCRIPT_DIR/clike/$test_name.cl"
    local input_file="$SCRIPT_DIR/clike/$test_name.in"
    local stdout_expect="$SCRIPT_DIR/clike/$test_name.out"
    local stderr_expect="$SCRIPT_DIR/clike/$test_name.err"
    local disasm_expect="$SCRIPT_DIR/clike/$test_name.disasm"
    local sqlite_marker="$SCRIPT_DIR/clike/$test_name.sqlite"
    local net_marker="$SCRIPT_DIR/clike/$test_name.net"

    local details=()
    local status="PASS"

    if [ "$test_name" = "graphics" ]; then
        if [ "$CLIKE_GRAPHICS_AVAILABLE" -ne 1 ]; then
            harness_report SKIP "clike_${test_name}" "CLike fixture $test_name" "Graphics builtins unavailable"
            return
        fi
        if [ "${RUN_SDL:-0}" != "1" ] && [ "${SDL_VIDEODRIVER:-}" = "dummy" ]; then
            harness_report SKIP "clike_${test_name}" "CLike fixture $test_name" "SDL disabled"
            return
        fi
    fi

    if [ -f "$sqlite_marker" ] && [ "$CLIKE_SQLITE_AVAILABLE" -ne 1 ]; then
        harness_report SKIP "clike_${test_name}" "CLike fixture $test_name" "SQLite builtins disabled"
        return
    fi

    if [ -f "$net_marker" ] && [ "${RUN_NET_TESTS:-0}" != "1" ]; then
        harness_report SKIP "clike_${test_name}" "CLike fixture $test_name" "Network tests disabled (set RUN_NET_TESTS=1)"
        return
    fi

    local actual_out=$(mktemp)
    local actual_err=$(mktemp)

    if [ -f "$disasm_expect" ]; then
        local disasm_stderr=$(mktemp)
        set +e
        run_clike_command --dump-bytecode-only "$src" > /dev/null 2> "$disasm_stderr"
        local disasm_status=$?
        set -e
        strip_ansi_inplace "$disasm_stderr"
        perl -0 -pe 's/^Compilation successful.*\n//m; s/^Loaded cached byte code.*\n//m' "$disasm_stderr" > "$disasm_stderr.clean"
        mv "$disasm_stderr.clean" "$disasm_stderr"
        rel_src="Tests/clike/$test_name.cl"
        sed -i.bak "s|$src|$rel_src|" "$disasm_stderr" 2>/dev/null || true
        rm -f "$disasm_stderr.bak"
        if [ $disasm_status -ne 0 ]; then
            status="FAIL"
            details+=("Disassembly exited with status $disasm_status")
            if [ -s "$disasm_stderr" ]; then
                details+=("Disassembly stderr:
$(cat "$disasm_stderr")")
            fi
        else
            set +e
            diff_output=$(diff -u "$disasm_expect" "$disasm_stderr")
            local diff_status=$?
            set -e
            if [ $diff_status -ne 0 ]; then
                status="FAIL"
                if [ $diff_status -eq 1 ]; then
                    details+=("Disassembly mismatch:
$diff_output")
                else
                    details+=("Disassembly diff failed with status $diff_status")
                fi
            fi
        fi
        rm -f "$disasm_stderr"
    fi

    local server_pid=""
    local server_ready_file=""
    local prev_http_port=""
    local had_prev_http_port=0

    if [ "${CLIKE_HTTP_TEST_PORT+x}" = x ]; then
        prev_http_port="$CLIKE_HTTP_TEST_PORT"
        had_prev_http_port=1
    fi

    if [ "$status" = "PASS" ] && [ -f "$net_marker" ] && [ "${RUN_NET_TESTS:-0}" = "1" ]; then
        local server_script="$SCRIPT_DIR/clike/$test_name.net"
        if [ -s "$server_script" ]; then
            server_ready_file=$(mktemp)
            rm -f "$server_ready_file"
            PSCAL_NET_READY_FILE="$server_ready_file" python3 "$server_script" &
            server_pid=$!
            disown "$server_pid" 2>/dev/null || true
            if ! wait_for_ready_file "$server_ready_file" 5; then
                status="FAIL"
                details+=("Failed to start helper server for $test_name (script $server_script)")
                if [ -n "$server_pid" ]; then
                    stop_server "$server_pid"
                    server_pid=""
                fi
            elif [ -f "$server_ready_file" ]; then
                while IFS='=' read -r key value; do
                    if [ "$key" = "PORT" ] && [ -n "$value" ]; then
                        CLIKE_HTTP_TEST_PORT="$value"
                        export CLIKE_HTTP_TEST_PORT
                    fi
                done < "$server_ready_file"
            fi
        fi
    fi

    if [ "$status" = "PASS" ]; then
        set +e
        if [ -f "$input_file" ]; then
            run_clike_command "$src" < "$input_file" > "$actual_out" 2> "$actual_err"
        else
            run_clike_command "$src" > "$actual_out" 2> "$actual_err"
        fi
        local run_status=$?
        set -e

        normalise_clike_stdout "$actual_out"
        normalise_clike_stderr "$actual_err"

        if [ -f "$stdout_expect" ]; then
            set +e
            diff_output=$(diff -u "$stdout_expect" "$actual_out")
            local diff_status=$?
            set -e
            if [ $diff_status -ne 0 ]; then
                status="FAIL"
                if [ $diff_status -eq 1 ]; then
                    details+=("stdout mismatch:
$diff_output")
                else
                    details+=("diff on stdout failed with status $diff_status")
                fi
            fi
        else
            if [ -s "$actual_out" ] && [ ! -f "$stderr_expect" ]; then
                status="FAIL"
                details+=("Unexpected stdout:
$(cat "$actual_out")")
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
                    details+=("stderr mismatch:
$diff_output")
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
                details+=("clike run exited with $run_status")
                if [ -s "$actual_err" ]; then
                    details+=("stderr:
$(cat "$actual_err")")
                fi
            elif [ -s "$actual_err" ]; then
                status="FAIL"
                details+=("Unexpected stderr:
$(cat "$actual_err")")
            fi
        fi
    fi

    if [ -n "$server_pid" ]; then
        stop_server "$server_pid"
    fi
    if [ "$had_prev_http_port" -eq 1 ]; then
        CLIKE_HTTP_TEST_PORT="$prev_http_port"
        export CLIKE_HTTP_TEST_PORT
    else
        unset CLIKE_HTTP_TEST_PORT
    fi
    if [ -n "$server_ready_file" ]; then
        rm -f "$server_ready_file"
    fi

    rm -f "$actual_out" "$actual_err"

    if [ "$status" = "PASS" ]; then
        harness_report PASS "clike_${test_name}" "CLike fixture $test_name"
    else
        harness_report FAIL "clike_${test_name}" "CLike fixture $test_name" "${details[@]}"
    fi
}

clike_cache_reuse_test() {
    local tmp_home src_dir
    tmp_home=$(mktemp -d)
    src_dir=$(mktemp -d)
    cat > "$src_dir/CacheTest.cl" <<'EOF'
int main() {
    printf("first\n");
    return 0;
}
EOF
    shift_mtime "$src_dir/CacheTest.cl" -5

    set +e
    (cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" CacheTest.cl > "$tmp_home/out1" 2> "$tmp_home/err1")
    local status1=$?
    set -e

    local issues=()
    if [ $status1 -ne 0 ]; then
        issues+=("Initial compile exited with $status1")
    elif ! grep -q 'first' "$tmp_home/out1"; then
        issues+=("Initial compile missing expected stdout")
    else
        set +e
        (cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" CacheTest.cl > "$tmp_home/out2" 2> "$tmp_home/err2")
        local status2=$?
        set -e
        if [ $status2 -ne 0 ]; then
            issues+=("Cached compile exited with $status2")
        elif ! grep -q 'Loaded cached byte code' "$tmp_home/err2"; then
            issues+=("Expected cached byte code notice missing")
        fi
    fi

    rm -rf "$tmp_home" "$src_dir"

    if [ ${#issues[@]} -eq 0 ]; then
        return 0
    fi

    printf '%s\n' "${issues[@]}"
    return 1
}

clike_cache_binary_staleness_test() {
    local tmp_home src_dir
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
    local status1=$?
    set -e

    local issues=()
    if [ $status1 -ne 0 ]; then
        issues+=("Initial compile exited with $status1")
    elif ! grep -q 'first' "$tmp_home/out1"; then
        issues+=("Initial compile missing expected stdout")
    else
        shift_mtime "$CLIKE_BIN" 5
        set +e
        (cd "$src_dir" && HOME="$tmp_home" "$CLIKE_BIN" BinaryTest.cl > "$tmp_home/out2" 2> "$tmp_home/err2")
        local status2=$?
        set -e
        if [ $status2 -ne 0 ]; then
            issues+=("Recompile after binary touch exited with $status2")
        elif grep -q 'Loaded cached byte code' "$tmp_home/err2"; then
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

TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"
trap 'rm -rf "$TEST_HOME"' EXIT

cd "$ROOT_DIR"

if details=$(run_clike_ext_builtin_dump clike); then
    harness_report PASS "clike_ext_builtin_dump" "clike --dump-ext-builtins validates structure"
else
    harness_report FAIL "clike_ext_builtin_dump" "clike --dump-ext-builtins validates structure" "$details"
fi

if details=$(clike_cli_version); then
    harness_report PASS "clike_cli_version" "clike -v reports latest tag"
else
    harness_report FAIL "clike_cli_version" "clike -v reports latest tag" "$details"
fi

if details=$(clike_cli_dump_ast_json); then
    harness_report PASS "clike_cli_dump_ast_json" "clike --dump-ast-json emits JSON"
else
    harness_report FAIL "clike_cli_dump_ast_json" "clike --dump-ast-json emits JSON" "$details"
fi

if details=$(clike_cli_vm_trace); then
    harness_report PASS "clike_cli_vm_trace" "clike --vm-trace-head produces trace"
else
    harness_report FAIL "clike_cli_vm_trace" "clike --vm-trace-head produces trace" "$details"
fi

if has_ext_builtin_category "$CLIKE_BIN" sqlite; then
    CLIKE_SQLITE_AVAILABLE=1
else
    CLIKE_SQLITE_AVAILABLE=0
fi

if has_ext_builtin_category "$CLIKE_BIN" graphics; then
    CLIKE_GRAPHICS_AVAILABLE=1
else
    CLIKE_GRAPHICS_AVAILABLE=0
fi

shopt -s nullglob
for src in "$SCRIPT_DIR"/clike/*.cl; do
    test_name=$(basename "$src" .cl)
    run_clike_fixture "$test_name"
done
shopt -u nullglob

if details=$(clike_cache_reuse_test); then
    harness_report PASS "clike_cache_reuse" "Cache reuse surfaces bytecode reuse notice"
else
    harness_report FAIL "clike_cache_reuse" "Cache reuse surfaces bytecode reuse notice" "$details"
fi

if details=$(clike_cache_binary_staleness_test); then
    harness_report PASS "clike_cache_binary_staleness" "Binary timestamp invalidates cached bytecode"
else
    harness_report FAIL "clike_cache_binary_staleness" "Binary timestamp invalidates cached bytecode" "$details"
fi

harness_summary "CLike"
if harness_exit_code; then
    exit 0
fi
exit 1
