#!/bin/bash
# Serve+eval queue for the cs-aug20 LARGER models, on claw2.
#
# Why claw2 and not claw3: these merges are 28-67 GB and claw3's usable window
# is ~27 GiB. claw2 is the host cs-aug4 served these same models from, via
# serve_any.sh, whose default is --gpu-memory-utilization 0.85. torch's
# mem_get_info reports only ~31.8 GiB "free" here, but that is a conservative
# figure on GB10 unified memory -- vLLM allocates well past it. 0.85 is the
# proven setting for this tier on this box, so no NVFP4 step is needed.
# (Contrast the 8B/9B grid on claw3, where 0.18 was right for 8B and 0.22 was
# needed for the 9B -- that constant is model-size- and host-specific.)
#
# enable_thinking=false is applied to the qwen36-* models as well as qwen35-*:
# Qwen3.6 is the same qwen3_5 / qwen3_5_moe lineage, and without the flag the
# raw "Thinking Process: ..." trace lands in the content field and is graded as
# the Aether program, which looks like catastrophic regression but is a harness
# bug. A post-suite leak check flags it immediately rather than after the fact.
set -u
GPU_UTIL="${GPU_UTIL:-0.85}"
# aether_doc_bench.py defaults --repair-attempts to 0. cs-aug4 ran with 1,
# so grading cs-aug20 at 0 gave the baseline a free extra shot at every
# failure. Run with 2; first-attempt-only numbers are still recoverable
# from the per-attempt records in the JSON.
REPAIR_ATTEMPTS="${REPAIR_ATTEMPTS:-2}"
BENCH_ROOT=$HOME/pscal-bench
QDIR=/storage/aether_eval_es2_claw2
LOG="$QDIR/queue.log"
TRA=${TRA_QUEUE_URL:?set TRA_QUEUE_URL (private fleet overlay)}
mkdir -p "$QDIR"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }

release() {
  local rc=$?
  log "RELEASE - handing claw2 back (exit rc=$rc)"
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1 || true
  for t in claw2_shared claw2_ollama; do
    curl -s --max-time 20 -X DELETE "$TRA/api/targets/$t/control" >/dev/null 2>&1 \
      && log "RELEASE - T'Ra $t released" || log "RELEASE - WARNING $t release failed"
  done
  log "RELEASE - complete"
}
trap release EXIT

for t in claw2_shared claw2_ollama; do
  curl -s --max-time 20 -X POST "$TRA/api/targets/$t/control" -H 'Content-Type: application/json' \
    -d '{"mode":"external_busy","owner":"claude-code cs-aug20-large","reason":"cs-aug20 larger-model eval on claw2 GB10","duration_seconds":43200}' >/dev/null 2>&1
done
log "RESERVE - claw2 targets marked external_busy"

TAGS=(
  qwen3-8b-nothink-8bit-cs-aug20-es2
)

declare -A SUITE_TASKS=(
  [simple]="Tests/aether_doc_bench/tasks_v2_pos.json"
  [large]="Tests/aether_doc_bench/tasks_hard.json"
  [cs]="Tests/aether_doc_bench/tasks_cs.json"
)

# Grader PINNED to c660b1b, the commit that graded the 8B/9B half of the
# cs-aug20 board. Another session pushed 13f241f + d05ee22 (BUILT-002
# compile-time builtin arity check, toon_parse_string alias) mid-experiment,
# which moved aether-current to 2026-07-26-2. Grading the two halves of one
# board with different compilers would show up as a model result, so pin.
GRADER=${GRADER:-$HOME/aether-c660b1b/build/aether}
log "GRADER $GRADER"
log "GRADER version: $($GRADER --version 2>&1 | head -1)"
log "GRADER commit:  $(git -C "$(dirname "$(dirname "$GRADER")")" log --oneline -1 2>&1)"
log "NOTE aether-current is at: $(git -C $HOME/aether-current log --oneline -1 2>&1) (NOT used for grading)"
[ -x "$GRADER" ] || { log "ABORT - grader missing at $GRADER"; exit 1; }

log "EVAL QUEUE START (claw2) - ${#TAGS[@]} models, suites=simple,large,cs docs=none reps=3 gpu_util=$GPU_UTIL"

for TAG in "${TAGS[@]}"; do
  DONE_MARK="$QDIR/${TAG}.done"
  [ -f "$DONE_MARK" ] && { log "SKIP $TAG - already evaluated"; continue; }
  MERGED="/storage/$TAG/merged_16bit"
  if [ ! -f "$MERGED/config.json" ]; then
    log "FAIL $TAG - no merged export at $MERGED, skipping"
    continue
  fi
  log "=== $TAG ($(du -sh "$MERGED" 2>/dev/null | cut -f1)) ==="

  # Mistral: the Unsloth merge drops tekken.json (Mistral's native tokenizer)
  # and rewrites tokenizer_config/special_tokens_map. Without tekken.json vLLM
  # falls back to the HF tokenizer.json path, which drops spaces in generated
  # text ("hello from benchmark" -> "hellofrombenchmark", "95 => " -> "95=>").
  # cs-aug4 hit this too: 16/35 broken vs 32/35 with the base tokenizer,
  # identical weights. NOTE tokenizer.json alone is byte-identical to base, so
  # comparing just that file does NOT detect this -- tekken.json is the tell.
  TOK_ARGS=()
  case "$TAG" in
    mistral24b-*)
      TOK_ARGS=(--tokenizer /storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724)
      log "  (mistral: serving with BASE tokenizer to avoid the Tekken space-dropping bug)"
      ;;
  esac

  READY=0
  for ATTEMPT in 1 2 3; do
    log "=== serving $TAG (attempt $ATTEMPT/3, util=$GPU_UTIL) ==="
    docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
    docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
      -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
      --model "$MERGED" --served-model-name "$TAG" \
      ${TOK_ARGS[@]+"${TOK_ARGS[@]}"} \
      --dtype bfloat16 --max-model-len 16384 --gpu-memory-utilization "$GPU_UTIL" \
      --enforce-eager --port 8000 >> "$LOG" 2>&1

    # big models load slowly off NFS: allow 30 min
    for i in $(seq 1 180); do
      curl -s --max-time 5 http://localhost:8019/v1/models 2>/dev/null | grep -q "\"$TAG\"" && { READY=1; break; }
      [ "$(docker inspect -f '{{.State.Running}}' aether-bench-vllm-q7 2>/dev/null)" = "false" ] && break
      sleep 10
    done
    [ "$READY" -eq 1 ] && break
    log "attempt $ATTEMPT/3 failed for $TAG - last 120 lines:"
    docker logs --tail 120 aether-bench-vllm-q7 >> "$LOG" 2>&1
    docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
    sleep 5
  done
  [ "$READY" -ne 1 ] && { log "FAIL $TAG - vLLM never became ready after 3 attempts"; continue; }
  log "READY $TAG"

  DEST_CFG="$BENCH_ROOT/Tests/aether_doc_bench/destinations.${TAG}_v2.json"
  EXTRA_BODY=""
  case "$TAG" in
    qwen35-9b-*|qwen36-*)
      EXTRA_BODY=',"extra_body":{"chat_template_kwargs":{"enable_thinking":false}}'
      log "  (qwen3_5/3_6 lineage: enable_thinking=false to suppress reasoning-trace leak)"
      ;;
  esac
  cat > "$DEST_CFG" <<JSON
{"destinations":[{"id":"${TAG}-v2","type":"openai_chat_completions","base_url":"http://localhost:8019/v1","api_key":"","model":"${TAG}","temperature":0,"max_output_tokens":2000,"request_timeout_seconds":300,"request_max_retries":3,"retry_backoff_seconds":3,"cooldown_seconds":0${EXTRA_BODY}}]}
JSON

  cd "$BENCH_ROOT"
  for SUITE in simple large cs; do
    log "--- eval $TAG suite=$SUITE ---"
    python3 -u tools/aether_doc_bench.py \
      --destinations-config "$DEST_CFG" \
      --tasks "${SUITE_TASKS[$SUITE]}" \
      --docs none --repeats 3 --progress \
      --repair-attempts "$REPAIR_ATTEMPTS" \
      --aether-bin "$GRADER" \
      --output-json "$QDIR/${TAG}_${SUITE}.json" --text-summary >> "$LOG" 2>&1

    # leak check: a reasoning trace graded as source is a harness bug, not a score
    python3 - "$QDIR/${TAG}_${SUITE}.json" <<'PY' 2>/dev/null | tee -a "$LOG"
import json,sys
try:
    d=json.load(open(sys.argv[1]))
    v=d["destinations"][0]["variants"][0]
    rs=v["results"]
    leak=sum(1 for r in rs if (r.get("source_code") or "").lstrip().lower().startswith(("thinking","<think","okay, ")))
    empty=sum(1 for r in rs if not (r.get("source_code") or "").strip())
    s=v["summary"]
    if leak: print(f"  !! LEAK CHECK: {leak}/{len(rs)} generations look like reasoning traces")
    if empty: print(f"  !! EMPTY: {empty}/{len(rs)} generations produced no source")
    print(f"  score: exact={s['exact_stdout_match']}/{s['total_cases']} gen_ok={s['generated_ok']}")
except Exception as e:
    print(f"  (leak check failed: {e})")
PY
  done

  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  touch "$DONE_MARK"
  log "=== DONE $TAG ==="
done

log "EVAL QUEUE COMPLETE"
touch "$QDIR/queue.done"
