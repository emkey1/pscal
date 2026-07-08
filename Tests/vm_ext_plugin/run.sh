#!/usr/bin/env bash
# VM 2.0 Phase 7 (Docs/pscal_vm2_plan.md §7.1) regression suite: the
# dlopen plugin ABI (backend_ast/pscal_ext_api.h, ext_builtins/
# plugin_loader.c) and the sqlite-as-plugin proof (components/pscal-core/
# plugins/sqlite/sqlite_ext_plugin.c). Requires build/bin/pascal and
# build/sqlite_ext_plugin.{dylib,so} to already be built (the latter is
# skipped, not failed, if sqlite support wasn't compiled in).
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
PASCAL_BIN="$ROOT_DIR/build/bin/pascal"

. "$SCRIPT_DIR/../tools/harness_utils.sh"
harness_init

if [ ! -x "$PASCAL_BIN" ]; then
    echo "pascal binary not found at $PASCAL_BIN" >&2
    exit 1
fi

case "$(uname -s)" in
    Darwin) PLUGIN_SUFFIX=".dylib" ;;
    *) PLUGIN_SUFFIX=".so" ;;
esac

SQLITE_PLUGIN="$ROOT_DIR/build/sqlite_ext_plugin${PLUGIN_SUFFIX}"
if [ ! -f "$SQLITE_PLUGIN" ]; then
    echo "sqlite_ext_plugin${PLUGIN_SUFFIX} not found at $SQLITE_PLUGIN -- sqlite support (ENABLE_EXT_BUILTIN_SQLITE) likely not built; skipping vm_ext_plugin suite." >&2
    exit 0
fi

CC="${CC:-cc}"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# --- Case 1: sqlite-as-plugin parity with the static in-tree category ---
static_out="$WORK_DIR/static.out"
plugin_out="$WORK_DIR/plugin.out"
"$PASCAL_BIN" --no-cache "$SCRIPT_DIR/sqlite_plugin_smoke.pas" > "$static_out" 2>&1
static_status=$?
"$PASCAL_BIN" --ext "$SQLITE_PLUGIN" --no-cache "$SCRIPT_DIR/sqlite_plugin_smoke.pas" > "$plugin_out" 2>&1
plugin_status=$?

if [ "$static_status" -ne 0 ] || [ "$plugin_status" -ne 0 ]; then
    harness_report FAIL "ext_plugin_sqlite_parity" "sqlite-as-plugin matches the static category" \
        "static exit=$static_status plugin exit=$plugin_status\nstatic:\n$(cat "$static_out")\nplugin:\n$(cat "$plugin_out")"
elif ! diff -q "$static_out" "$plugin_out" > /dev/null 2>&1; then
    harness_report FAIL "ext_plugin_sqlite_parity" "sqlite-as-plugin matches the static category" \
        "output mismatch:\n$(diff -u "$static_out" "$plugin_out")"
else
    harness_report PASS "ext_plugin_sqlite_parity" "sqlite-as-plugin matches the static category"
fi

# --- Case 2: PSCAL_EXT_DIR directory scan finds and loads the same plugin ---
ext_dir="$WORK_DIR/ext_dir"
mkdir -p "$ext_dir"
cp "$SQLITE_PLUGIN" "$ext_dir/"
dir_out="$WORK_DIR/dir.out"
PSCAL_EXT_DIR="$ext_dir" "$PASCAL_BIN" --no-cache "$SCRIPT_DIR/sqlite_plugin_smoke.pas" > "$dir_out" 2>&1
dir_status=$?
if [ "$dir_status" -ne 0 ] || ! diff -q "$static_out" "$dir_out" > /dev/null 2>&1; then
    harness_report FAIL "ext_plugin_dir_scan" "PSCAL_EXT_DIR loads a plugin the same way --ext does" \
        "exit=$dir_status\n$(cat "$dir_out")"
else
    harness_report PASS "ext_plugin_dir_scan" "PSCAL_EXT_DIR loads a plugin the same way --ext does"
fi

# --- Case 3: the --deny ext / PSCAL_VM_DENY=ext runtime gate (VM 2.0
# Phase 7 follow-up) blocks plugin loading cleanly, both via --ext and
# PSCAL_EXT_DIR, composes with --deny all, and does NOT over-trigger on an
# unrelated deny class. ---
deny_ext_out="$WORK_DIR/deny_ext.out"
"$PASCAL_BIN" --deny ext --ext "$SQLITE_PLUGIN" --no-cache "$SCRIPT_DIR/sqlite_plugin_smoke.pas" > "$deny_ext_out" 2>&1
deny_ext_status=$?
if [ "$deny_ext_status" -ne 1 ] || ! grep -qF "denied by --deny/PSCAL_VM_DENY policy (ext)" "$deny_ext_out"; then
    harness_report FAIL "ext_plugin_deny_ext_flag" "--deny ext blocks --ext cleanly" \
        "exit=$deny_ext_status\n$(cat "$deny_ext_out")"
else
    harness_report PASS "ext_plugin_deny_ext_flag" "--deny ext blocks --ext cleanly"
fi

deny_ext_dir_out="$WORK_DIR/deny_ext_dir.out"
PSCAL_EXT_DIR="$ext_dir" "$PASCAL_BIN" --deny ext --no-cache "$SCRIPT_DIR/sqlite_plugin_smoke.pas" > "$deny_ext_dir_out" 2>&1
deny_ext_dir_status=$?
if [ "$deny_ext_dir_status" -ne 1 ] || ! grep -qF "denied by --deny/PSCAL_VM_DENY policy (ext)" "$deny_ext_dir_out"; then
    harness_report FAIL "ext_plugin_deny_ext_env" "--deny ext blocks PSCAL_EXT_DIR cleanly" \
        "exit=$deny_ext_dir_status\n$(cat "$deny_ext_dir_out")"
else
    harness_report PASS "ext_plugin_deny_ext_env" "--deny ext blocks PSCAL_EXT_DIR cleanly"
fi

deny_all_out="$WORK_DIR/deny_all.out"
"$PASCAL_BIN" --deny all --ext "$SQLITE_PLUGIN" --no-cache "$SCRIPT_DIR/sqlite_plugin_smoke.pas" > "$deny_all_out" 2>&1
deny_all_status=$?
if [ "$deny_all_status" -ne 1 ] || ! grep -qF "denied by --deny/PSCAL_VM_DENY policy (ext)" "$deny_all_out"; then
    harness_report FAIL "ext_plugin_deny_all_includes_ext" "--deny all also blocks plugin loading" \
        "exit=$deny_all_status\n$(cat "$deny_all_out")"
else
    harness_report PASS "ext_plugin_deny_all_includes_ext" "--deny all also blocks plugin loading"
fi

deny_net_out="$WORK_DIR/deny_net.out"
"$PASCAL_BIN" --deny net --ext "$SQLITE_PLUGIN" --no-cache "$SCRIPT_DIR/sqlite_plugin_smoke.pas" > "$deny_net_out" 2>&1
deny_net_status=$?
if [ "$deny_net_status" -ne 0 ] || ! diff -q "$static_out" "$deny_net_out" > /dev/null 2>&1; then
    harness_report FAIL "ext_plugin_deny_net_does_not_block_ext" "an unrelated --deny class does not block plugin loading" \
        "exit=$deny_net_status\n$(cat "$deny_net_out")"
else
    harness_report PASS "ext_plugin_deny_net_does_not_block_ext" "an unrelated --deny class does not block plugin loading"
fi

# --- Adversarial cases: each must fail cleanly (host exits normally with
# status 1, never crashes/hangs itself) with a diagnostic naming the
# actual problem. A trivial pass-through program is enough -- these cases
# test the loader, not the plugin's own functionality.
trivial_pas="$WORK_DIR/trivial.pas"
cat > "$trivial_pas" <<'EOF'
program Trivial;
begin
  writeln('ok');
end.
EOF

run_adversarial_case() {
    local case_id="$1" description="$2" plugin_path="$3" expect_substring="$4"
    local out="$WORK_DIR/${case_id}.out"
    "$PASCAL_BIN" --ext "$plugin_path" --no-cache "$trivial_pas" > "$out" 2>&1
    local status=$?
    if [ "$status" -ne 1 ]; then
        harness_report FAIL "$case_id" "$description" \
            "expected clean exit status 1 (got $status -- a signal-terminated status like 139 would mean the HOST crashed, not just the plugin load failing):\n$(cat "$out")"
    elif ! grep -qF "$expect_substring" "$out"; then
        harness_report FAIL "$case_id" "$description" \
            "diagnostic missing expected substring '$expect_substring':\n$(cat "$out")"
    else
        harness_report PASS "$case_id" "$description"
    fi
}

# Case 3: corrupt/truncated shared library (not a valid Mach-O/ELF).
corrupt_lib="$WORK_DIR/corrupt${PLUGIN_SUFFIX}"
head -c 64 "$SQLITE_PLUGIN" > "$corrupt_lib"
run_adversarial_case "ext_plugin_corrupt_library" \
    "a truncated/corrupt shared library fails cleanly, never crashes the host" \
    "$corrupt_lib" "dlopen"

# Case 4: valid library missing the pscal_ext_register entry point.
no_entry_lib="$WORK_DIR/no_entry${PLUGIN_SUFFIX}"
"$CC" -shared -fPIC -o "$no_entry_lib" "$SCRIPT_DIR/fixtures/no_entry_point.c" 2>"$WORK_DIR/no_entry_cc.log"
if [ ! -f "$no_entry_lib" ]; then
    harness_report FAIL "ext_plugin_missing_entry_point" "a valid library missing pscal_ext_register fails cleanly" \
        "fixture failed to compile:\n$(cat "$WORK_DIR/no_entry_cc.log")"
else
    run_adversarial_case "ext_plugin_missing_entry_point" \
        "a valid library missing pscal_ext_register fails cleanly" \
        "$no_entry_lib" "missing '${PSCAL_EXT_ENTRY_POINT_NAME:-pscal_ext_register}' entry point"
fi

# Case 5: entry point reports an incompatible host ABI (real version check,
# not an arbitrary sentinel -- see fixtures/reject_wrong_abi.c).
reject_abi_lib="$WORK_DIR/reject_abi${PLUGIN_SUFFIX}"
"$CC" -shared -fPIC -I "$ROOT_DIR/components/pscal-core/src" \
    -o "$reject_abi_lib" "$SCRIPT_DIR/fixtures/reject_wrong_abi.c" 2>"$WORK_DIR/reject_abi_cc.log"
if [ ! -f "$reject_abi_lib" ]; then
    harness_report FAIL "ext_plugin_abi_mismatch" "an ABI-major-mismatched plugin is rejected cleanly" \
        "fixture failed to compile:\n$(cat "$WORK_DIR/reject_abi_cc.log")"
else
    run_adversarial_case "ext_plugin_abi_mismatch" \
        "an ABI-major-mismatched plugin is rejected cleanly" \
        "$reject_abi_lib" "entry point rejected registration"
fi

# Case 6: entry point segfaults.
crash_segv_lib="$WORK_DIR/crash_segv${PLUGIN_SUFFIX}"
"$CC" -shared -fPIC -o "$crash_segv_lib" "$SCRIPT_DIR/fixtures/crash_segv.c" 2>"$WORK_DIR/crash_segv_cc.log"
if [ ! -f "$crash_segv_lib" ]; then
    harness_report FAIL "ext_plugin_crash_segv" "a segfaulting entry point is isolated, never crashes the host" \
        "fixture failed to compile:\n$(cat "$WORK_DIR/crash_segv_cc.log")"
else
    run_adversarial_case "ext_plugin_crash_segv" \
        "a segfaulting entry point is isolated, never crashes the host" \
        "$crash_segv_lib" "plugin crashed while loading"
fi

# Case 7: entry point calls abort() (a distinct crash class/signal from segv).
crash_abort_lib="$WORK_DIR/crash_abort${PLUGIN_SUFFIX}"
"$CC" -shared -fPIC -o "$crash_abort_lib" "$SCRIPT_DIR/fixtures/crash_abort.c" 2>"$WORK_DIR/crash_abort_cc.log"
if [ ! -f "$crash_abort_lib" ]; then
    harness_report FAIL "ext_plugin_crash_abort" "an aborting entry point is isolated, never crashes the host" \
        "fixture failed to compile:\n$(cat "$WORK_DIR/crash_abort_cc.log")"
else
    run_adversarial_case "ext_plugin_crash_abort" \
        "an aborting entry point is isolated, never crashes the host" \
        "$crash_abort_lib" "plugin crashed while loading"
fi

# --- Case 8: a nonexistent --ext path fails at CLI-parse time (fast, no
# fork/dlopen attempt needed) ---
missing_out="$WORK_DIR/missing.out"
"$PASCAL_BIN" --ext "$WORK_DIR/does_not_exist${PLUGIN_SUFFIX}" --no-cache "$trivial_pas" > "$missing_out" 2>&1
missing_status=$?
if [ "$missing_status" -ne 1 ] || ! grep -qF "cannot open" "$missing_out"; then
    harness_report FAIL "ext_plugin_missing_path" "a nonexistent --ext path is rejected at CLI-parse time" \
        "exit=$missing_status\n$(cat "$missing_out")"
else
    harness_report PASS "ext_plugin_missing_path" "a nonexistent --ext path is rejected at CLI-parse time"
fi

harness_summary "vm_ext_plugin"
harness_exit_code
