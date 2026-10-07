#!/bin/bash
# Run one destination through the frontier trio, into the tier board's output
# directory and under its naming convention.
#
#   bash Tests/aether_doc_bench/bench_one_destination.sh <destination-id>
#
# Use to pull a single model forward out of a long sequential lane. Because the
# output names match the main driver's, that driver will [skip] the destination
# when it reaches it -- no duplicate work, no coordination needed.
#
# This is a separate file on purpose: bash reads a script incrementally as it
# executes, so editing the main driver while it is running risks corrupting the
# running instance. The validation below is a deliberate copy of the driver's.
set -uo pipefail

DEST=${1:?usage: bench_one_destination.sh <destination-id>}
CFG=Tests/aether_doc_bench/destinations.local_tiers_20260811.json
OUTDIR=${OUTDIR:-Tests/aether_doc_bench/results/local_tiers_20260811}
AETHER_BIN=${AETHER_BIN:-/usr/local/bin/aether}
# The harness runs each case from a temp cwd, so a relative --aether-bin
# resolves to nothing and every compile fails with ENOENT. Make it absolute
# here rather than trusting the caller (runbook pre-flight #1).
case "$AETHER_BIN" in /*) ;; *) AETHER_BIN="$PWD/$AETHER_BIN" ;; esac
SUITES=${SUITES:-"tasks_frontier tasks_frontier_algo tasks_frontier_spec"}
REPAIR=${REPAIR:-2}

mkdir -p "$OUTDIR/logs"

# Guides. DOCS selects the variants (default medium). DOC overrides their files,
# space-separated NAME=PATH, and for a board points at the docs/ of the checkout
# that built AETHER_BIN, so binary and guide share one sha:
#   DOC="medium=$HOME/aether-<sha>/docs/aether_for_llms_medium_contexts.md"
DOCS=${DOCS:-medium}
DOC_ARGS=()
for spec in ${DOC:-}; do DOC_ARGS+=(--doc "$spec"); done
# Anything else for the harness, word-split: --allow-skew, --aether-bin-sha256 HEX,
# --aether-root DIR, --seed-base 42, --repeats 3, --aether-arg=FLAG, ...
read -r -a EXTRA_ARGS <<< "${BENCH_ARGS:-}"
# bash 3.2 (macOS) treats "${arr[@]}" of an empty array as unbound under set -u.
HARNESS_ARGS=(--docs "$DOCS" ${DOC_ARGS[@]+"${DOC_ARGS[@]}"} --aether-bin "$AETHER_BIN" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

# Preflight. The harness copies the binary, hashes it, and refuses one it cannot
# tie to the guides' checkout (the skew guard: a -dirty build, a '+' gitlink, a
# +sha other than the checkout's HEAD, or a standalone build without
# --allow-skew and its sha). This replaces the old pinned-version date gate,
# which compared only the date and could not see any of those.
PREFLIGHT_LOG="$OUTDIR/logs/${DEST}__preflight.log"
if ! python3 tools/aether_doc_bench.py \
        --destinations-config "$CFG" \
        --destination "$DEST" \
        --tasks "Tests/aether_doc_bench/${SUITES%% *}.json" \
        "${HARNESS_ARGS[@]}" \
        --preflight-only >"$PREFLIGHT_LOG" 2>&1; then
    echo "FATAL: harness preflight refused the toolchain -- see $PREFLIGHT_LOG"
    sed 's/^/    /' "$PREFLIGHT_LOG"
    exit 1
fi
echo "[preflight] $(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("aether", d["aether_version"], "sha256", d["binary_sha256"][:16])' "$PREFLIGHT_LOG") | destination $DEST"

for suite in $SUITES; do
    name="${DEST}__${suite}"
    out="$OUTDIR/${name}.json"
    log="$OUTDIR/logs/${name}.log"

    if [ -s "$out" ]; then echo "[skip] $name"; continue; fi

    echo "[run ] $name"
    started=$SECONDS
    python3 tools/aether_doc_bench.py \
        --destinations-config "$CFG" \
        --destination "$DEST" \
        --tasks "Tests/aether_doc_bench/$suite.json" \
        "${HARNESS_ARGS[@]}" \
        --repair-attempts "$REPAIR" \
        --output-json "$out.tmp" \
        --text-summary --progress >"$log" 2>&1
    rc=$?
    elapsed=$(( SECONDS - started ))

    # rc 3: some cases failed in the infrastructure and measured nothing. They
    # are re-run (never dropped, never scored) before the run is judged.
    if [ $rc -eq 3 ] && [ -s "$out.tmp" ]; then
        echo "[infra] $name: re-running the cases that measured nothing"
        python3 Tests/aether_doc_bench/rerun_nogen_cases.py --report "$out.tmp" --apply >>"$log" 2>&1
        rc=$?
        if [ $rc -ne 0 ]; then
            mkdir -p "$OUTDIR/infra_pending"; mv "$out.tmp" "$OUTDIR/infra_pending/${name}.json"
            echo "[FAIL] $name -- infra-failed cases remain; kept in infra_pending/ (rerun_nogen_cases.py --report)"
            continue
        fi
    fi

    if [ $rc -ne 0 ] || [ ! -s "$out.tmp" ]; then
        rm -f "$out.tmp"; echo "[FAIL] $name rc=$rc ${elapsed}s -- see $log"; continue
    fi

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
# Explicit provider/HTTP-level messages only, and only on generation failures.
# This list used to match "timed out" anywhere, so a program that ran past its
# time limit (now fingerprinted timeout:<task>, a real measurement) counted
# toward rejecting the variant as a transport outage.
TRANSPORT = ("http api error", "http api request failed", "http api request timed out",
             "provider request exceeded", "tra_queue unreachable", "not done within deadline",
             "connection refused", "connection reset", "reset by peer", "unreachable",
             " 502", " 503", " 504")
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
        fingerprint = str(fp.get("fingerprint", "")).lower()
        if fingerprint.startswith("generation:") and any(t in fingerprint for t in TRANSPORT):
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
        ACCEPT*) mv "$out.tmp" "$out"; echo "[done] $name rc=0 ${elapsed}s ${verdict#ACCEPT}" ;;
        *) mkdir -p "$OUTDIR/rejected"; mv "$out.tmp" "$OUTDIR/rejected/${name}.json"
           echo "[FAIL] $name rc=0 ${elapsed}s -- $verdict (quarantined, will re-run)" ;;
    esac
done

echo "[$DEST] COMPLETE"
