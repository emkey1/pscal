#!/bin/bash
# Run one cloud-spectrum destination through the frontier trio.
#
#   bash Tests/aether_doc_bench/run_cloud_spectrum.sh <destination-id>
#
# Exports GEMINI_API_KEY and OPENAI_API_KEY by reading the existing gitignored
# configs that already carry them, so the keys reach the harness through the
# environment only: never on a command line (visible in `ps`), never in a file
# this repo tracks, never written anywhere. The credential files are read and
# never modified.
set -uo pipefail

DEST=${1:?usage: run_cloud_spectrum.sh <destination-id>}
CFG=Tests/aether_doc_bench/destinations.cloud_spectrum_20260811.json
OUTDIR=Tests/aether_doc_bench/results/local_tiers_20260811
AETHER_BIN=${AETHER_BIN:-/usr/local/bin/aether}
case "$AETHER_BIN" in /*) ;; *) AETHER_BIN="$PWD/$AETHER_BIN" ;; esac
SUITES=${SUITES:-"tasks_frontier tasks_frontier_algo tasks_frontier_spec"}
REPAIR=${REPAIR:-2}

mkdir -p "$OUTDIR/logs"

PIN_FILE=Tests/aether_doc_bench/toolchain/PINNED_VERSION
if [ -f "$PIN_FILE" ]; then WANT=$(cat "$PIN_FILE"); PIN_SRC="pin"; else WANT=$(cat components/aether/VERSION); PIN_SRC="VERSION"; fi
GOT=$("$AETHER_BIN" --version 2>&1 | sed -n 's/.*Version: \([0-9-]*\).*/\1/p')
[ "$WANT" != "$GOT" ] && { echo "FATAL: $AETHER_BIN is $GOT, $PIN_SRC says $WANT"; exit 1; }

# Read-only credential lift. Values are assigned inside this process; they are
# never echoed and never passed as arguments.
export GEMINI_API_KEY=$(python3 -c "
import json
print(json.load(open('Tests/aether_doc_bench/destinations.gemini_lowtier.json'))['destinations'][0]['api_key'])")
export OPENAI_API_KEY=$(python3 -c "
import json
print(json.load(open('Tests/aether_doc_bench/destinations.openai_lowmid.json'))['destinations'][0]['api_key'])")
[ -z "$GEMINI_API_KEY" ] && { echo "FATAL: GEMINI_API_KEY empty"; exit 1; }
[ -z "$OPENAI_API_KEY" ] && { echo "FATAL: OPENAI_API_KEY empty"; exit 1; }
echo "[preflight] aether $GOT | destination $DEST | keys loaded (gemini ${#GEMINI_API_KEY}c, openai ${#OPENAI_API_KEY}c)"

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
        --docs medium \
        --repair-attempts "$REPAIR" \
        --aether-bin "$AETHER_BIN" \
        --output-json "$out.tmp" \
        --text-summary --progress >"$log" 2>&1
    rc=$?
    elapsed=$(( SECONDS - started ))

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
TRANSPORT = ("timed out", "timeout", "http api request failed", "connection",
             "refused", "unreachable", "reset by peer", " 502", " 503", " 504",
             " 429", "rate limit", " 401", " 404")
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
