#!/bin/bash
# Local-model tier board, first run of the post-2026-08-10 fresh era.
#
# Four lanes in parallel, sequential within each lane. The lanes are the
# physical serving nodes, not the tiers: m5t can only hold one LM Studio model
# at a time, so its eight models must queue, while ds4 (claw1) and ornith
# (claw3) are dedicated deployments that run concurrently with everything.
#
#   A  ds4          claw1:8900, direct        high
#   B  ornith-35b   T'Ra -> claw3_ornith      high
#   C  m5t          T'Ra -> m5_remote         mid x4 + low x4
#   D  m2t          T'Ra -> m2_remote         low x1
#
# Resumable: one output file per destination-suite, skipped if already present
# and non-empty. Written to .tmp and moved into place only on success, so a
# killed run never leaves a truncated file that the skip check would honour --
# the exact failure that made the 2026-07-05 Ornith cs run look complete.
#
# Run from the repo root under tmux + caffeinate. Never a foreground harness
# call: these lanes run for hours.
set -uo pipefail

CFG=Tests/aether_doc_bench/destinations.local_tiers_20260811.json
OUTDIR=Tests/aether_doc_bench/results/local_tiers_20260811
AETHER_BIN=${AETHER_BIN:-/usr/local/bin/aether}
# The harness runs each case from a temp cwd, so a relative --aether-bin
# resolves to nothing and every compile fails with ENOENT. Make it absolute
# here rather than trusting the caller (runbook pre-flight #1).
case "$AETHER_BIN" in /*) ;; *) AETHER_BIN="$PWD/$AETHER_BIN" ;; esac
SUITES="tasks_frontier tasks_frontier_algo tasks_frontier_spec"
REPAIR=2

mkdir -p "$OUTDIR/logs"

# Gate on the compiler the board is PINNED to, not on the live VERSION file.
# components/aether moves while a board runs (a separate session landed compiler
# fixes and a VERSION bump mid-run on 2026-08-11), and a board must hold its
# toolchain still or its rows stop being comparable with each other.
PIN_FILE=Tests/aether_doc_bench/toolchain/PINNED_VERSION
if [ -f "$PIN_FILE" ]; then
    WANT_VERSION=$(cat "$PIN_FILE"); PIN_SRC="pin"
else
    WANT_VERSION=$(cat components/aether/VERSION); PIN_SRC="components/aether/VERSION"
fi
GOT_VERSION=$("$AETHER_BIN" --version 2>&1 | sed -n 's/.*Version: \([0-9-]*\).*/\1/p')
if [ "$WANT_VERSION" != "$GOT_VERSION" ]; then
    echo "FATAL: $AETHER_BIN is $GOT_VERSION but $PIN_SRC says $WANT_VERSION"
    exit 1
fi
echo "[preflight] aether $GOT_VERSION at $AETHER_BIN"
echo "[preflight] outdir $OUTDIR"

run_one() {
    local dest=$1 suite=$2 tag=${3:-} ; shift 2; [ $# -gt 0 ] && shift
    local name="${dest}__${suite}${tag}"
    local out="$OUTDIR/${name}.json"
    local log="$OUTDIR/logs/${name}.log"

    if [ -s "$out" ]; then
        echo "[skip] $name"
        return 0
    fi

    echo "[run ] $name"
    local started=$SECONDS
    python3 tools/aether_doc_bench.py \
        --destinations-config "$CFG" \
        --destination "$dest" \
        --tasks "Tests/aether_doc_bench/$suite.json" \
        --docs medium \
        --repair-attempts "$REPAIR" \
        --aether-bin "$AETHER_BIN" \
        --output-json "$out.tmp" \
        --text-summary --progress \
        "$@" >"$log" 2>&1
    local rc=$?
    local elapsed=$(( SECONDS - started ))

    if [ $rc -ne 0 ] || [ ! -s "$out.tmp" ]; then
        rm -f "$out.tmp"
        echo "[FAIL] $name rc=$rc ${elapsed}s -- see $log"
        return 0
    fi

    # A non-zero exit and a non-empty file are NOT enough. When a target is
    # unreachable the harness still exits 0 and still writes a well-formed
    # report -- with zero variants, or with variants whose generated_ok is 0.
    # Accepting those is how a serving outage becomes a permanent 2/15 in the
    # record, and how skip-if-exists then refuses to re-run it.
    local verdict
    verdict=$(python3 - "$out.tmp" <<'PYEOF'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception as e:
    print(f"REJECT unparseable: {e}"); raise SystemExit(0)
dests = d.get("destinations") or []
variants = dests[0].get("variants") if dests else []
if not variants:
    print("REJECT zero variants (target unreachable / preflight skip)"); raise SystemExit(0)
# Transport failures are never model results. A model that writes bad Aether
# still produced bytes; one behind a dead socket produced nothing, and scoring
# that as capability is how a network outage becomes a permanent low score.
TRANSPORT = ("timed out", "timeout", "http api request failed", "connection",
             "refused", "unreachable", "reset by peer", " 502", " 503", " 504")
notes = []
for v in variants:
    s = v.get("summary", {})
    tot, gen = s.get("total_cases", 0), s.get("generated_ok", 0)
    if tot == 0:
        print(f"REJECT variant {v.get('doc_name')} has zero cases"); raise SystemExit(0)
    if gen == 0:
        print(f"REJECT variant {v.get('doc_name')} generated 0/{tot} -- serving failure, not a score")
        raise SystemExit(0)
    hit = 0
    for fp in v.get("failure_patterns", []):
        if any(t in str(fp.get("fingerprint", "")).lower() for t in TRANSPORT):
            hit += int(fp.get("count", 0))
    # Proportional, not absolute. An outage looks like most of the suite failing
    # in transport; a single blip is one unmeasured case in an otherwise good
    # run, and discarding 13 valid measurements to avoid 1 bad one is worse.
    if tot and hit / tot > 0.25:
        print(f"REJECT variant {v.get('doc_name')}: {hit}/{tot} cases failed in transport, not generation")
        raise SystemExit(0)
    if hit:
        notes.append(f"{v.get('doc_name')} {hit}/{tot} UNMEASURED (transport)")
    if gen < tot:
        notes.append(f"{v.get('doc_name')} gen_ok={gen}/{tot}")
print("ACCEPT" + (" PARTIAL " + ", ".join(notes) if notes else ""))
PYEOF
)

    case "$verdict" in
        ACCEPT*)
            mv "$out.tmp" "$out"
            echo "[done] $name rc=0 ${elapsed}s ${verdict#ACCEPT}"
            ;;
        *)
            mkdir -p "$OUTDIR/rejected"
            mv "$out.tmp" "$OUTDIR/rejected/${name}.json"
            echo "[FAIL] $name rc=0 ${elapsed}s -- $verdict (quarantined, will re-run)"
            ;;
    esac
    return 0
}

lane() {
    local lane_name=$1; shift
    echo "[lane ] $lane_name starting: $*"
    for dest in "$@"; do
        for suite in $SUITES; do
            run_one "$dest" "$suite"
        done
    done
    echo "[lane ] $lane_name COMPLETE"
}

# --- Lane A: ds4, the only direct-hit lane -----------------------------------
# Also the only lane that can honour --repeats: T'Ra's idempotency key returns
# one cached job N times instead of N samples, so retry-rate is measurable here
# and nowhere else on this board.
lane_ds4() {
    lane "A/ds4" high-ds4
    run_one high-ds4 tasks_frontier_spec "_r3" --repeats 3
    echo "[lane ] A/ds4 COMPLETE (incl. spec repeats)"
}

lane_ds4                                                              > "$OUTDIR/logs/lane_a_ds4.log" 2>&1 &
PID_A=$!
lane "B/ornith" high-ornith-35b                                       > "$OUTDIR/logs/lane_b_ornith.log" 2>&1 &
PID_B=$!
lane "C/m5t" mid-qwen36-35b-a3b mid-glm47-flash mid-devstral-small-2 \
             mid-qwen36-27b low-qwen35-9b low-ornith-9b \
             low-prism-coder-7b low-gemma4-e4b                        > "$OUTDIR/logs/lane_c_m5t.log" 2>&1 &
PID_C=$!
lane "D/m2t" low-granite4-h-tiny                                      > "$OUTDIR/logs/lane_d_m2t.log" 2>&1 &
PID_D=$!

echo "[main ] lanes running: A=$PID_A B=$PID_B C=$PID_C D=$PID_D"
wait $PID_A; echo "[main ] lane A exited"
wait $PID_B; echo "[main ] lane B exited"
wait $PID_C; echo "[main ] lane C exited"
wait $PID_D; echo "[main ] lane D exited"

echo "[main ] ALL LANES COMPLETE"
ls -la "$OUTDIR"/*.json 2>/dev/null | wc -l | xargs echo "[main ] result files:"
