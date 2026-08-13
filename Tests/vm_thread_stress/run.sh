#!/usr/bin/env bash
# VM 2.0 Phase 5a/5b (Docs/pscal_vm2_plan.md Sec 6.1/6.2) task/channel
# concurrency regression suite. Requires build/bin/pascal and
# build/bin/clike already built. Not previously wired into any automated
# runner -- the fixtures in this directory existed (written during Phase
# 5a/5b's own stress verification) but nothing ran them automatically,
# same gap class as Tests/vm_fx_policy before it was wired in.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
PASCAL_BIN="$ROOT_DIR/build/bin/pascal"
CLIKE_BIN="$ROOT_DIR/build/bin/clike"

. "$SCRIPT_DIR/../tools/harness_utils.sh"
harness_init

if [ ! -x "$PASCAL_BIN" ]; then
    echo "pascal binary not found at $PASCAL_BIN" >&2
    exit 1
fi

# name -> expect ("pass" = exit 0 required; "error" = nonzero exit required,
# by design -- see the fixture's own header comment)
declare -a PASCAL_FIXTURES=(
    "channel_close_wakes_all:pass"
    "http_async_task_stress:pass"
    "task_cancel_race:pass"
    "task_concurrent_spawn_await:pass"
    "channel_mpmc_stress:pass"
    "task_nested_spawn:pass"
    "channel_basic_smoke:pass"
    "channel_spsc_backpressure:pass"
    "channel_task_cancel_wakes_receive:pass"
    "channel_send_after_close_errors:error"
)

for entry in "${PASCAL_FIXTURES[@]}"; do
    name="${entry%%:*}"
    expect="${entry##*:}"
    src="$SCRIPT_DIR/$name.pas"
    if [ ! -f "$src" ]; then
        harness_report FAIL "vm_thread_stress_$name" "Pascal fixture $name" "missing $src"
        continue
    fi
    out="$(mktemp)"
    "$PASCAL_BIN" --no-cache "$src" > "$out" 2>&1
    status=$?
    if [ "$expect" = "pass" ]; then
        if [ "$status" -eq 0 ]; then
            harness_report PASS "vm_thread_stress_$name" "Pascal fixture $name"
        else
            harness_report FAIL "vm_thread_stress_$name" "Pascal fixture $name" \
                "expected exit 0, got $status:\n$(cat "$out")"
        fi
    else
        if [ "$status" -ne 0 ]; then
            harness_report PASS "vm_thread_stress_$name" "Pascal fixture $name (expected-error case)"
        else
            harness_report FAIL "vm_thread_stress_$name" "Pascal fixture $name (expected-error case)" \
                "expected nonzero exit (this program is designed to abort), got 0:\n$(cat "$out")"
        fi
    fi
    rm -f "$out"
done

if [ -x "$CLIKE_BIN" ]; then
    src="$SCRIPT_DIR/task_channel_clike.cl"
    out="$(mktemp)"
    "$CLIKE_BIN" --no-cache "$src" > "$out" 2>&1
    status=$?
    if [ "$status" -eq 0 ] && grep -q "^OK$" "$out"; then
        harness_report PASS "vm_thread_stress_task_channel_clike" "CLike task/channel fixture"
    else
        harness_report FAIL "vm_thread_stress_task_channel_clike" "CLike task/channel fixture" \
            "exit=$status:\n$(cat "$out")"
    fi
    rm -f "$out"
else
    harness_report SKIP "vm_thread_stress_task_channel_clike" "CLike task/channel fixture" \
        "clike binary not found at $CLIKE_BIN"
fi

harness_summary "vm_thread_stress"
harness_exit_code
exit $?
