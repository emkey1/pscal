#!/bin/bash
# Free LM Studio memory on m5t as the tier board's lane C advances.
#
# Lane C walks eight models sequentially through T'Ra -> m5_remote. LM Studio
# JIT-loads each one and does NOT evict: the eight sum to roughly 151 GB on a
# 128 GB laptop, so without eviction the run wedges partway through.
#
# Lane C is strictly sequential, so a destination that has finished all its
# suites is dead weight and can be unloaded safely. That is the only thing this
# script unloads. It never touches:
#   - a model whose destination still has suites to run (in use, or about to be)
#   - a model on a federated remote device (DEVICE != Local); those live on m2t
#     and are that node's business
#
# Deliberately conservative: it would rather leave memory allocated than unload
# a model mid-generation and score a transport failure.
set -uo pipefail

CFG=Tests/aether_doc_bench/destinations.local_tiers_20260811.json
OUTDIR=Tests/aether_doc_bench/results/local_tiers_20260811
LANE_LOG="$OUTDIR/logs/lane_c_m5t.log"
N_SUITES=3
POLL=${POLL:-120}

echo "[unloader] watching $LANE_LOG (poll ${POLL}s, $N_SUITES suites per destination)"

while true; do
    # Destination -> model, straight from the config so the two cannot drift.
    while IFS=$'\t' read -r dest model; do
        [ -z "$dest" ] && continue

        # Finished = it produced a terminal line for every suite.
        # NB: grep -c prints "0" AND exits 1 when there are no matches, so a
        # `|| echo 0` here would yield "0\n0" and blow up the comparison below.
        n_done=$(grep -cE "^\[(done|FAIL)\] ${dest}__" "$LANE_LOG" 2>/dev/null) || n_done=0
        [ -z "$n_done" ] && n_done=0
        [ "$n_done" -lt "$N_SUITES" ] && continue

        # Only unload if LM Studio still holds it, on THIS machine, and idle.
        if lms ps 2>/dev/null | awk -v m="$model" '$1==m && $0 ~ /Local/ && /IDLE/ {found=1} END{exit !found}'; then
            echo "[unloader] $dest complete ($n_done/$N_SUITES) -- unloading $model"
            lms unload "$model" 2>&1 | sed 's/^/[unloader]   /'
        fi
    done < <(python3 -c "
import json
d=json.load(open('$CFG'))
for x in d['destinations']:
    if 'm5_remote' in (x.get('preferred_targets') or []):
        print(x['id']+chr(9)+x['model'])
")

    if ! tmux has-session -t bench_tiers 2>/dev/null; then
        echo '[unloader] board finished -- final sweep of Local models'
        while IFS=$'\t' read -r dest model; do
            [ -z "$dest" ] && continue
            if lms ps 2>/dev/null | awk -v m="$model" '$1==m && $0 ~ /Local/ {found=1} END{exit !found}'; then
                echo "[unloader] unloading $model"
                lms unload "$model" 2>&1 | sed 's/^/[unloader]   /'
            fi
        done < <(python3 -c "
import json
d=json.load(open('$CFG'))
for x in d['destinations']:
    if 'm5_remote' in (x.get('preferred_targets') or []):
        print(x['id']+chr(9)+x['model'])
")
        echo '[unloader] done'
        lms ps 2>&1 | sed 's/^/[unloader] /'
        exit 0
    fi

    sleep "$POLL"
done
