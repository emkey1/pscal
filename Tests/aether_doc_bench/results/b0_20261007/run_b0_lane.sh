#!/bin/bash
# One sequential lane of the B0 baseline boards (plan W1-19, section 6.2).
#
#   bash Tests/aether_doc_bench/results/b0_20261007/run_b0_lane.sh <lane>
#
# lane = local  B0 (medium, G panel) -> B1 (medium + card, G panel, traps)
#               -> B0-small (small, 16K cohort) -> P0 (card, G panel) -> S0 (medium, 1 model, scale)
#        cloud  B0-full (full, 2 frontier models)
#
# B0, B0-small and P0 read the task manifests frozen in tasks_frozen/ (the exact
# files the board started on; later umbrella commits only add reference
# solutions to them, which the model never sees). B1 and S0 read the live
# Tests/aether_doc_bench manifests, which those suites first appeared in.
#
# Every G-panel model is served by one T'Ra target, which holds one model at a
# time, so the local lane runs strictly one request after another; running
# lanes per model would only make T'Ra swap models on every job.
#
# Unit of work = (board, destination, suite, repeat), one report each, named
# <dest>__<suite>__r<R>.json. A unit whose report exists is skipped, so a killed
# lane resumes where it stopped, and a unit killed mid-way continues from its
# checkpoint (--resume keeps finished cases). Repeat R is requested with seed 42+R
# (--start-repeat R --repeats 1). Infra-failed cases (harness rc 3) are re-run
# with rerun_nogen_cases.py until none remain or 3 rounds pass; a unit that still
# has some is parked in infra_pending/ and reported, never dropped.
#
# The harness asks T'Ra to unload a destination's model when a unit is done
# with it; the lane passes --keep-loaded when the next unit uses the same
# destination, so a model is loaded once per run of consecutive units.
#
# Environment (all optional): AETHER_ROOT (default ~/aether-6659679),
# AETHER_SHA256, LOCAL_CFG, CLOUD_CFG, CLOUD_DESTS, PANEL, COHORT16K, S0_DESTS,
# RUN_DIR (results directory of this board set; default results/b0_20261007 -- the
# OpenRouter twin uses results/b0_openrouter_20261010 with LOCAL_CFG pointing at
# destinations.openrouter_20261010.json and no TRA_QUEUE_URL),
# BOARDS (which boards to run, default all) and REPEATS (repeat indices, default "0 1 2"),
# BP=1 (adds the Python baseline board), MAXPAR (rolling slots: units in flight at once on one model; default 1), PYTHON (3.10+), LANE_TAG (names the status/DONE files, so a second local lane
# on another T'Ra endpoint -- e.g. PANEL=mid-bonsai2-27b-ternary COHORT16K= S0_DESTS=
# LANE_TAG=local_m2 -- keeps its own log).
#
# Run it on a host that never sleeps: a laptop in clamshell sleep turns every
# request it fires during a maintenance wake into an infra-failed case.
set -u

LANE=${1:?usage: run_b0_lane.sh local|cloud}
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$ROOT" || exit 1

HERE=${RUN_DIR:-Tests/aether_doc_bench/results/b0_20261007}
AETHER_ROOT=${AETHER_ROOT:-$HOME/aether-6659679}
BIN=$AETHER_ROOT/build/aether
AETHER_SHA256=${AETHER_SHA256:-addfeb8da341b1b6f344ab9ad13daba6eb6478623b5b35e2a5ec79555acc9b66}
LOCAL_CFG=${LOCAL_CFG:-Tests/aether_doc_bench/destinations.b0.local.json}
CLOUD_CFG=${CLOUD_CFG:-Tests/aether_doc_bench/destinations.cloud_spectrum_20260811.json}
PANEL=${PANEL:-"mid-qwen36-35b-a3b mid-devstral-small-2 mid-qwen36-27b low-qwen35-9b low-prism-coder-7b"}
COHORT16K=${COHORT16K-"low-qwen35-9b"}
CLOUD_DESTS=${CLOUD_DESTS:-"cloud-gemini-3.6-flash cloud-gemini-3.5-flash-lite"}
B0_SUITES="tasks_v2_pos tasks_hard_v2 tasks_hard_nontoon tasks_cs"
P0_SUITES="tasks_v2_pos tasks_cs"
FULL_SUITES="tasks_v2_pos tasks_hard_v2"
B1_SUITES="tasks_traps"
S0_SUITES="tasks_scale"
S0_DESTS=${S0_DESTS-"mid-qwen36-35b-a3b"}
DOCS_DIR=$AETHER_ROOT/docs

PYTHON=${PYTHON:-python3}
COMMON=(--aether-bin "$BIN" --aether-root "$AETHER_ROOT" --allow-skew
        --aether-bin-sha256 "$AETHER_SHA256" --repair-attempts 2)

TAG=${LANE_TAG:-$LANE}
STATUS=$HERE/status_$TAG.log
note() { echo "$(date -u +%FT%TZ) $*" | tee -a "$STATUS"; }

# A dead scheduler would turn every case of a unit into an infra failure (and
# the re-run rounds would burn on it too), so the local lane waits for T'Ra to
# answer before each unit and each re-run round. TRA_QUEUE_URL comes from the
# private fleet.env; when it is unset the check is skipped.
wait_tra() {
    [ "$LANE" = local ] && [ -n "${TRA_QUEUE_URL:-}" ] || return 0
    local waited=0
    until curl -sf -m 10 -o /dev/null "$TRA_QUEUE_URL/api/targets"; do
        [ $waited -eq 0 ] && note "[wait] scheduler not answering; pausing until it does"
        waited=$((waited + 60)); sleep 60
    done
    [ $waited -gt 0 ] && note "[wait] scheduler back after ${waited}s"
    return 0
}

# tasks_for BOARD SUITE: the manifest a board reads.
tasks_for() {
    case "$1" in
        B0|B0-small|P0|B0-full|BP) echo "$HERE/tasks_frozen/$2.json" ;;
        *) echo "Tests/aether_doc_bench/$2.json" ;;
    esac
}

# DOCNAME and DOCFILE may be comma lists ("medium,card" / "a.md,b.md"): the
# variants then run in one report, interleaved per task.
doc_args() {
    local IFS=,
    local names=($1) files=($2) i
    DOCARGS=(--docs "$1")
    for i in "${!names[@]}"; do
        # "-" (or nothing) means a variant without a guide file, e.g. none.
        case "${files[$i]:--}" in -) ;; *) DOCARGS+=(--doc "${names[$i]}=${files[$i]}") ;; esac
    done
}

# run_unit BOARD DOCNAME DOCFILE CFG DEST SUITE REPEAT SEEDARGS...
run_unit() {
    local board=$1 docname=$2 docfile=$3 cfg=$4 dest=$5 suite=$6 r=$7; shift 7
    local dir=$HERE/$board name="${dest}__${suite}__r${r}"
    local out=$dir/$name.json log=$dir/logs/$name.log
    mkdir -p "$dir/logs"
    if [ -s "$out" ]; then note "[skip] $board $name"; return 0; fi
    wait_tra
    note "[run ] $board $name"
    local started=$SECONDS
    doc_args "$docname" "$docfile"
    "$PYTHON" tools/aether_doc_bench.py \
        --destinations-config "$cfg" --destination "$dest" \
        --tasks "$(tasks_for "$board" "$suite")" \
        "${DOCARGS[@]}" \
        "${COMMON[@]}" "$@" \
        --start-repeat "$r" --repeats 1 --resume \
        --output-json "$out.tmp" --text-summary --progress >>"$log" 2>&1
    local rc=$?
    local round=0
    while [ $rc -eq 3 ] && [ -s "$out.tmp" ] && [ $round -lt 3 ]; do
        round=$((round + 1))
        wait_tra
        note "[infra] $board $name: rerun_nogen_cases round $round"
        "$PYTHON" Tests/aether_doc_bench/rerun_nogen_cases.py --report "$out.tmp" --apply >>"$log" 2>&1
        rc=$?
    done
    if [ $rc -eq 3 ] && [ -s "$out.tmp" ]; then
        mkdir -p "$dir/infra_pending"; mv "$out.tmp" "$dir/infra_pending/$name.json"
        note "[FAIL] $board $name: infra-failed cases remain, parked in infra_pending/"
        return 1
    fi
    if [ $rc -ne 0 ] || [ ! -s "$out.tmp" ]; then
        note "[FAIL] $board $name rc=$rc $((SECONDS - started))s, see $log"
        rm -f "$out.tmp"; return 1
    fi
    # A run that generated nothing is a serving failure, not a score.
    local verdict
    verdict=$("$PYTHON" - "$out.tmp" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
vs = (d.get("destinations") or [{}])[0].get("variants") or []
if not vs:
    print("REJECT zero variants"); raise SystemExit
parts = []
for v in vs:
    s = v["summary"]
    if not s.get("total_cases") and v.get("python_baseline_summary"):
        s = v["python_baseline_summary"]  # --skip-aether --python-baseline (board BP)
    tot, gen = s.get("total_cases", 0), s.get("generated_ok", 0)
    if tot == 0 or gen == 0:
        print(f"REJECT {v.get('doc_name')} generated {gen}/{tot}"); raise SystemExit
    parts.append(f"{v.get('doc_name')} fa={s.get('first_attempt_exact')}/{tot} fx={s.get('final_exact')}/{tot} gen={gen}")
print("ACCEPT " + "; ".join(parts))
PYEOF
)
    case "$verdict" in
        ACCEPT*) mv "$out.tmp" "$out"; note "[done] $board $name $((SECONDS - started))s ${verdict#ACCEPT }" ;;
        *) mkdir -p "$dir/rejected"; mv "$out.tmp" "$dir/rejected/$name.json"
           note "[FAIL] $board $name: $verdict (quarantined in rejected/)"; return 1 ;;
    esac
}

preflight() {  # BOARD DOCNAME DOCFILE CFG DEST SUITE
    case " ${BOARDS:-B0 B1 B0-small P0 S0 BP B0-full} " in *" $1 "*) ;; *) return 0 ;; esac
    local log=$HERE/$1/logs/${5}__preflight.log
    mkdir -p "$HERE/$1/logs"
    doc_args "$2" "$3"
    if ! "$PYTHON" tools/aether_doc_bench.py --destinations-config "$4" --destination "$5" \
            --tasks "$(tasks_for "$1" "$6")" "${DOCARGS[@]}" \
            "${COMMON[@]}" --preflight-only >"$log" 2>&1; then
        note "[FATAL] preflight refused $1 $5, see $log"; return 1
    fi
}

note "lane $LANE start, binary $AETHER_SHA256"

# The lane's units, in run order: "board doc docfile cfg dest suite repeat seedarg".
UNITS=()
# BOARDS limits a run to some boards; REPEATS to some repeat indices (seed 42+r).
BOARDS=${BOARDS:-"B0 B1 B0-small P0 S0 BP B0-full"}
REPEATS=${REPEATS:-"0 1 2"}
unit() { case " $BOARDS " in *" $1 "*) UNITS+=("$*") ;; esac; }
case "$LANE" in
local)
    MED=$DOCS_DIR/aether_for_llms_medium_contexts.md
    SMALL=$DOCS_DIR/aether_for_llms_with_small_contexts.md
    CARD=$DOCS_DIR/aether_card.md
    for d in $PANEL; do preflight B0 medium "$MED" "$LOCAL_CFG" "$d" tasks_v2_pos || exit 1; done
    for d in $COHORT16K; do preflight B0-small small "$SMALL" "$LOCAL_CFG" "$d" tasks_v2_pos || exit 1; done
    for d in $PANEL; do preflight P0 card "$CARD" "$LOCAL_CFG" "$d" tasks_v2_pos || exit 1; done
    for d in $PANEL; do preflight B1 medium,card "$MED,$CARD" "$LOCAL_CFG" "$d" tasks_traps || exit 1; done
    for d in $S0_DESTS; do preflight S0 medium "$MED" "$LOCAL_CFG" "$d" tasks_scale || exit 1; done
    # Model-major (model, repeat, suite) since 2026-10-09: with rolling slots
    # (MAXPAR) a model's 12 units keep all slots busy on one loaded model. Before
    # that it was repeat-major, which put a single-repeat board first.
    for d in $PANEL; do for r in $REPEATS; do for s in $B0_SUITES; do
        unit B0 medium "$MED" "$LOCAL_CFG" "$d" "$s" "$r" --seed-base=42; done; done; done
    # B1 next: G1 waits for B0 and B1 (plan D3), and its card arm is P0's traps part.
    # From B1 on the order is model-major (model, repeat, suite), so a model's units
    # sit together and MAXPAR can run them side by side on one loaded model.
    for d in $PANEL; do for r in $REPEATS; do for s in $B1_SUITES; do
        unit B1 medium,card "$MED,$CARD" "$LOCAL_CFG" "$d" "$s" "$r" --seed-base=42; done; done; done
    for d in $COHORT16K; do for r in $REPEATS; do for s in $B0_SUITES; do
        unit B0-small small "$SMALL" "$LOCAL_CFG" "$d" "$s" "$r" --seed-base=42; done; done; done
    for d in $PANEL; do for r in $REPEATS; do for s in $P0_SUITES; do
        unit P0 card "$CARD" "$LOCAL_CFG" "$d" "$s" "$r" --seed-base=42; done; done; done
    for d in $S0_DESTS; do for r in $REPEATS; do for s in $S0_SUITES; do
        unit S0 medium "$MED" "$LOCAL_CFG" "$d" "$s" "$r" --seed-base=42; done; done; done
    # BP: the same models solve B0's tasks in Python (no guide), as a reference for
    # how far Aether is from a language the models know. Gated (BP=1) until the
    # harness runs model-written Python sandboxed.
    if [ "${BP:-0}" = 1 ]; then
        for d in $PANEL; do preflight BP none - "$LOCAL_CFG" "$d" tasks_v2_pos || exit 1; done
        for d in $PANEL; do for r in $REPEATS; do for s in $B0_SUITES; do
            unit BP none - "$LOCAL_CFG" "$d" "$s" "$r" --seed-base=42 --skip-aether --python-baseline; done; done; done
    fi
    ;;
cloud)
    # The key comes from the environment only (the destinations use api_key_env).
    if [ -z "${GEMINI_API_KEY:-}" ]; then note "[FATAL] GEMINI_API_KEY is not set"; exit 1; fi
    FULL=$DOCS_DIR/aether_for_llms_and_others.md
    for d in $CLOUD_DESTS; do preflight B0-full full "$FULL" "$CLOUD_CFG" "$d" tasks_v2_pos || exit 1; done
    # Cloud x1 and no seed: a cloud destination is seeded only if it opts in.
    for d in $CLOUD_DESTS; do for s in $FULL_SUITES; do
        unit B0-full full "$FULL" "$CLOUD_CFG" "$d" "$s" 0 --keep-loaded; done; done
    ;;
*) echo "unknown lane $LANE"; exit 2 ;;
esac

# Rolling slots: up to MAXPAR units run at once, all on the same destination (one
# model loaded, several requests in flight; the T'Ra target's
# max_concurrent_requests must allow it). A unit starts as soon as a slot frees;
# a unit on a different destination first waits for the running ones to drain,
# so only one model is ever in use. A unit whose harness is already running (from
# an earlier driver, e.g. across a driver hand-off) is adopted: it holds a slot
# until that process ends, and is not started twice. --keep-loaded goes to every
# unit followed by one on the same destination; the last of a run asks T'Ra to
# release the model (refused while a sibling still uses it; T'Ra also unloads
# idle models when it switches).
MAXPAR=${MAXPAR:-1}
SLOT_PIDS=()
SLOT_DEST=""
WARMED=""
NRUN=0
reap() {
    local alive=() p
    for p in ${SLOT_PIDS[@]+"${SLOT_PIDS[@]}"}; do
        if kill -0 "$p" 2>/dev/null; then alive+=("$p"); else wait "$p" 2>/dev/null; fi
    done
    SLOT_PIDS=(${alive[@]+"${alive[@]}"})
    NRUN=${#SLOT_PIDS[@]}
}
# The pid that owns a unit already in flight: its harness's parent (the earlier
# driver's unit subshell) when that is a lane driver, else the harness itself.
owner_of() {
    local h pp
    h=$(pgrep -f -- "--output-json $1" | head -1)
    [ -n "$h" ] || return 1
    pp=$(ps -o ppid= -p "$h" | tr -d ' ')
    if [ -n "$pp" ] && ps -o command= -p "$pp" | grep -q run_b0_lane.sh; then echo "$pp"; else echo "$h"; fi
}
n=${#UNITS[@]}
for ((i = 0; i < n; i++)); do
    read -r -a u <<< "${UNITS[$i]}"
    name="${u[4]}__${u[5]}__r${u[6]}"
    if [ -s "$HERE/${u[0]}/$name.json" ]; then note "[skip] ${u[0]} $name"; continue; fi
    keep=()
    if [ $((i + 1)) -lt $n ]; then
        read -r -a nxt <<< "${UNITS[$((i + 1))]}"
        [ "${nxt[4]}" = "${u[4]}" ] && keep=(--keep-loaded)
    fi
    if [ "$MAXPAR" -le 1 ]; then
        run_unit "${u[@]}" ${keep[@]+"${keep[@]}"}
        continue
    fi
    reap
    if [ "$NRUN" -gt 0 ] && [ "$SLOT_DEST" != "${u[4]}" ]; then
        note "[drain] waiting for ${SLOT_DEST} units before ${u[4]}"
        while reap; [ "$NRUN" -gt 0 ]; do sleep "${SLOT_POLL:-15}"; done
    fi
    while reap; [ "$NRUN" -ge "$MAXPAR" ]; do sleep "${SLOT_POLL:-15}"; done
    SLOT_DEST=${u[4]}
    if owner=$(owner_of "$HERE/${u[0]}/$name.json.tmp"); then
        note "[adopt] ${u[0]} $name already running (pid $owner)"
        SLOT_PIDS+=("$owner")
        [ -s "$HERE/${u[0]}/$name.json.tmp" ] && WARMED=${u[4]}
        continue
    fi
    run_unit "${u[@]}" ${keep[@]+"${keep[@]}"} &
    SLOT_PIDS+=("$!")
    # The first unit on a destination runs alone until it has finished a case (its
    # checkpoint or report exists): the model is then loaded and answering. Starting
    # all slots at once made every unit ask for the model load together, and the
    # server refused the loads (2026-10-11: seven units rejected with zero variants).
    if [ "$WARMED" != "${u[4]}" ]; then
        wp=$!; wt=0
        while kill -0 "$wp" 2>/dev/null && [ ! -s "$HERE/${u[0]}/$name.json.tmp" ] \
                && [ ! -s "$HERE/${u[0]}/$name.json" ] && [ "$wt" -lt "${WARM_MAX:-3600}" ]; do
            sleep 5; wt=$((wt + 5))
        done
        if [ -s "$HERE/${u[0]}/$name.json.tmp" ] || [ -s "$HERE/${u[0]}/$name.json" ]; then
            WARMED=${u[4]}; note "[warm] ${u[4]} is answering after ${wt}s; opening $MAXPAR slots"
        else
            note "[warm] $name gave no case in ${wt}s; the next ${u[4]} unit warms again"
        fi
    fi
done
while reap; [ "$NRUN" -gt 0 ]; do sleep "${SLOT_POLL:-15}"; done
note "lane $LANE COMPLETE"
touch "$HERE/DONE_$TAG"
