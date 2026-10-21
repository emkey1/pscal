#!/usr/bin/env bash
# VM 2.0 Phase 3/4j stack-growth and copy-on-write stress (Docs/pscal_vm2_plan.md
# §5.9, §5.10.5). Like Tests/vm_thread_stress before it, nothing ran these
# automatically, so mt_static_cow_race.pas -- written as a stop-ship check --
# regressed unnoticed (pscal-core 1183157, fixed in the commit that added this
# runner). Requires build/bin/pascal.
#
# A fixture with a .pas.out must print exactly that; one without must exit 0
# and print a "PASS" line. The race fixture runs RACE_RUNS times (default 3):
# a single clean run of a race proves little.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
PASCAL_BIN="${PASCAL_BIN:-$ROOT_DIR/build/bin/pascal}"
RACE_RUNS="${RACE_RUNS:-3}"

. "$SCRIPT_DIR/../tools/harness_utils.sh"
harness_init

if [ ! -x "$PASCAL_BIN" ]; then
    echo "pascal binary not found at $PASCAL_BIN" >&2
    exit 1
fi

for src in "$SCRIPT_DIR"/*.pas; do
    name="$(basename "$src" .pas)"
    expect="$src.out"
    runs=1
    case "$name" in
        mt_*) runs="$RACE_RUNS" ;;
    esac
    status=PASS
    detail=""
    for run in $(seq 1 "$runs"); do
        out="$(mktemp)"
        "$PASCAL_BIN" --no-cache "$src" > "$out" 2>&1
        rc=$?
        if [ -f "$expect" ]; then
            if [ $rc -ne 0 ] || ! diff -q "$expect" "$out" > /dev/null; then
                status=FAIL
                detail="run $run: exit $rc, output differs from $(basename "$expect"):\n$(diff "$expect" "$out" | head -20)"
            fi
        elif [ $rc -ne 0 ] || ! grep -q "^PASS" "$out"; then
            status=FAIL
            detail="run $run: exit $rc, no PASS line:\n$(tail -20 "$out")"
        fi
        rm -f "$out"
        [ "$status" = FAIL ] && break
    done
    if [ "$status" = PASS ]; then
        harness_report PASS "vm_stack_growth_$name" "Pascal fixture $name ($runs run(s))"
    else
        harness_report FAIL "vm_stack_growth_$name" "Pascal fixture $name" "$detail"
    fi
done

harness_summary "vm_stack_growth_stress"
harness_exit_code
exit $?
