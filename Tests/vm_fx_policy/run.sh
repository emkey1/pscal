#!/usr/bin/env bash
# VM 2.0 Phase 6 (Docs/pscal_vm2_plan.md §6.3) regression suite: the --deny
# sandbox and the --fx-record/--fx-replay journal. Requires build/bin/pascal
# to already be built. Not previously wired into any automated runner --
# Tests/vm_fx_policy/replay_http_file.p existed but nothing ran it.
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

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# --- Case 1: --deny net rejects a network builtin cleanly ---------------
deny_out="$WORK_DIR/deny.out"
cat > "$WORK_DIR/net_test.pas" <<'EOF'
program NetTest;
var sess: integer;
begin
  sess := HttpSession();
  writeln('got session: ', sess);
end.
EOF
"$PASCAL_BIN" --deny net --no-cache "$WORK_DIR/net_test.pas" > "$deny_out" 2>&1
deny_status=$?
if [ "$deny_status" -eq 0 ]; then
    harness_report FAIL "vm_fx_deny_net" "--deny net rejects a network builtin" \
        "expected nonzero exit, got 0:\n$(cat "$deny_out")"
elif ! grep -q "denied by --deny/PSCAL_VM_DENY policy" "$deny_out"; then
    harness_report FAIL "vm_fx_deny_net" "--deny net rejects a network builtin" \
        "wrong diagnostic:\n$(cat "$deny_out")"
else
    harness_report PASS "vm_fx_deny_net" "--deny net rejects a network builtin"
fi

# --- Case 2: record -> mutate live state -> replay must match record ------
export VM_FX_TEST_TMP="$WORK_DIR"
export VM_FX_HTTP_TARGET="$WORK_DIR/http_target.txt"
echo "original content" > "$VM_FX_HTTP_TARGET"

record_out="$WORK_DIR/record.out"
"$PASCAL_BIN" --fx-record "$WORK_DIR/journal.fx" --no-cache "$SCRIPT_DIR/replay_http_file.p" > "$record_out" 2>&1
record_status=$?

echo "MUTATED between record and replay -- replay must not see this" > "$VM_FX_HTTP_TARGET"

replay_out="$WORK_DIR/replay.out"
"$PASCAL_BIN" --fx-replay "$WORK_DIR/journal.fx" --no-cache "$SCRIPT_DIR/replay_http_file.p" > "$replay_out" 2>&1
replay_status=$?

if [ "$record_status" -ne 0 ] || [ "$replay_status" -ne 0 ]; then
    harness_report FAIL "vm_fx_record_replay_match" "record/replay round-trip is immune to live-state mutation" \
        "record exit=$record_status replay exit=$replay_status\nrecord:\n$(cat "$record_out")\nreplay:\n$(cat "$replay_out")"
elif ! diff -q "$record_out" "$replay_out" > /dev/null 2>&1; then
    harness_report FAIL "vm_fx_record_replay_match" "record/replay round-trip is immune to live-state mutation" \
        "output mismatch (replay likely re-fetched live state instead of using the journal):\n$(diff -u "$record_out" "$replay_out")"
else
    harness_report PASS "vm_fx_record_replay_match" "record/replay round-trip is immune to live-state mutation"
fi

# --- Case 3: replay against a truncated/corrupt journal aborts cleanly ---
truncated_out="$WORK_DIR/truncated.out"
head -c 20 "$WORK_DIR/journal.fx" > "$WORK_DIR/journal_truncated.fx" 2>/dev/null
"$PASCAL_BIN" --fx-replay "$WORK_DIR/journal_truncated.fx" --no-cache "$SCRIPT_DIR/replay_http_file.p" > "$truncated_out" 2>&1
truncated_status=$?

if [ "$truncated_status" -eq 0 ]; then
    harness_report FAIL "vm_fx_replay_corrupt_journal" "replay against a truncated journal aborts cleanly" \
        "expected nonzero exit, got 0:\n$(cat "$truncated_out")"
elif ! grep -q "fx replay journal mismatch" "$truncated_out"; then
    harness_report FAIL "vm_fx_replay_corrupt_journal" "replay against a truncated journal aborts cleanly" \
        "expected a journal-mismatch diagnostic:\n$(cat "$truncated_out")"
else
    harness_report PASS "vm_fx_replay_corrupt_journal" "replay against a truncated journal aborts cleanly"
fi

harness_summary "vm_fx_policy"
harness_exit_code
exit $?
