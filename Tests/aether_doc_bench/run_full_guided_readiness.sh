#!/usr/bin/env bash
# Readiness check for the full guided-benchmark rerun (no jobs submitted).
# Confirms: aether binary built, task sets present, T'Ra queue healthy, and each
# cohort model is ROUTABLE right now (POST /jobs/explain dry-run). Flags any
# model that isn't served/routable so it can be loaded before the real run.
set -u
cd /Users/mke/PBuild || exit 1
BIN=/Users/mke/PBuild/components/aether/build/aether
DEST_CONFIG="${1:-Tests/aether_doc_bench/destinations.tra.json}"
SCHED="${TRA_QUEUE_URL:-http://100.121.116.25:8793}"

echo "=== binary ==="
if [ -x "$BIN" ]; then echo "OK  $BIN -> $($BIN --version 2>&1 | head -1)"; else echo "MISSING $BIN (rebuild post-fixes)"; fi
echo "=== task sets ==="
for f in tasks_v2_pos.json tasks_hard.json tasks_cs.json; do
  p="Tests/aether_doc_bench/$f"; [ -f "$p" ] && echo "OK  $p" || echo "MISSING $p"
done
echo "=== queue health ==="
curl -s -m 8 "$SCHED/health" >/dev/null 2>&1 && echo "OK  $SCHED" || echo "UNREACHABLE $SCHED"
echo "=== per-model routability (explain dry-run) ==="
python3 - "$DEST_CONFIG" "$SCHED" <<'PY'
import json,sys,urllib.request
cfg,sched=sys.argv[1],sys.argv[2]
ds=json.load(open(cfg))["destinations"]
ok=bad=0
for d in ds:
    eb=d.get("extra_body") or {}
    payload={"model":d.get("model"),"prompt":"x","max_tokens":64}
    if eb.get("preferred_targets"): payload["preferred_targets"]=eb["preferred_targets"]
    body={"resource_group":"llm","type":"llm_generate","priority":4,"submitter":"readiness","payload":payload}
    try:
        req=urllib.request.Request(f"{sched}/jobs/explain",data=json.dumps(body).encode(),headers={"Content-Type":"application/json"})
        r=json.load(urllib.request.urlopen(req,timeout=20))
        routable=r.get("routable"); note=r.get("scheduler_note")
        # busy is transient/fine; only a hard no-route is a real gap
        hard = (not routable) and str(note or "").split(":",1)[0]=="no_route_for_model"
        tag = "OK " if routable else ("BUSY(ok)" if not hard else "NO-ROUTE")
        print(f"  {tag:9} {d['id']:<30} model={d.get('model'):<28} sel={r.get('selected_target')} note={note}")
        ok += 1 if (routable or not hard) else 0; bad += 1 if hard else 0
    except Exception as e:
        print(f"  ERR       {d['id']:<30} {e}"); bad+=1
print(f"\nroutable-or-transient: {ok}/{len(ds)} | hard no-route: {bad}")
PY
