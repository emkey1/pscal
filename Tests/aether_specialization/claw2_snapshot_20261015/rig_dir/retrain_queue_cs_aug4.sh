#!/bin/bash
# Sequential Aether cs_aug4 retrain queue on the single GB10 GPU (claw2).
# Clone of the proven retrain_queue_cs_aug3.sh, extended to all 7 models seen
# across the cs-aug2-builtins and cs-aug3 boards (a3b-coder30b, mistral24b,
# qwen14b, qwen3-8b-nothink, qwen36-27b, q36, deepseek6.7b), all on the SAME
# corpus (data_cs_aug4, dataset 2026-07-15-1, aether 2026-07-09-1 post the
# setTypeValue recursion-crash fix) for the first time. Ordered small->large
# by base parameter count so early failures surface fast.
# Resumable (skips models whose merged export exists); detached; set -u not -e.
set -u

W=$HOME/training/aether-qwen-coder-30b-unsloth
DATA=data_cs_aug4
QDIR=/storage/aether_retrain_queue_cs_aug4
QLOG="$QDIR/queue.log"
mkdir -p "$QDIR"

log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QLOG"; }

A3B_BASE=/slows/archive/qwen3-coder-30b-a3b-model-mirror
[ -f "$A3B_BASE/config.json" ] || A3B_BASE=$(ls -d /storage/hf/hub/models--Qwen--Qwen3-Coder-30B-A3B-Instruct/snapshots/*/ 2>/dev/null | head -1)

# tag | image | base-model-path | 4bit(1/0) | r | alpha | epochs | INSTR_PART | RESP_PART
QUEUE=(
"deepseek6.7b-cs-aug4|aether-unsloth-qwen3-coder-30b:q3|/storage/hf/hub/models--deepseek-ai--deepseek-coder-6.7b-instruct/snapshots/e5d64addd26a6a1db0f9b863abf6ee3141936807|1|32|64|3|### Instruction:|### Response:"
"qwen3-8b-nothink-cs-aug4|aether-unsloth-qwen3-coder-30b:q3|/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218|1|32|64|3||"
"qwen14b-cs-aug4|aether-unsloth-qwen3-coder-30b:q3|/storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671|1|64|128|4||"
"mistral24b-cs-aug4|aether-unsloth-qwen3-coder-30b:q3|/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724|1|64|128|4|[INST]|[/INST]"
"qwen36-27b-cs-aug4|aether-unsloth-qwen3-coder-30b:base26-fla|/storage/models/Qwen3.6-27B|0|64|128|4||"
"a3b-coder30b-cs-aug4|aether-unsloth-qwen3-coder-30b:q3|${A3B_BASE:-/nonexistent}|1|32|64|3||"
"q36-cs-aug4|aether-unsloth-qwen3-coder-30b:base26-fla|/storage/models/Qwen3.6-35B-A3B-Instruct|1|32|64|3||"
)

log "QUEUE START — ${#QUEUE[@]} models, data=$DATA (dataset 2026-07-15-1 / aether 2026-07-09-1, post setTypeValue fix)"

for row in "${QUEUE[@]}"; do
  IFS="|" read -r TAG IMG MODEL FOURBIT R ALPHA EP IPART RPART <<< "$row"
  C="aether-train-$TAG"
  MERGED="/storage/$TAG/merged_16bit"

  log "=== model: $TAG (img=$IMG r=$R a=$ALPHA ep=$EP 4bit=$FOURBIT) ==="

  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    log "SKIP $TAG — merged export already exists ($MERGED)"
    continue
  fi
  if [ ! -f "$MODEL/config.json" ]; then
    log "FAIL $TAG — base model missing: $MODEL (skipping)"
    continue
  fi

  QBIT=()
  [ "$FOURBIT" = "0" ] && QBIT+=(--no-load-in-4bit)

  EXTRA=()
  if [ -n "$IPART" ]; then EXTRA+=(--instruction-part "$IPART" --response-part "$RPART"); fi

  docker rm -f "$C" >/dev/null 2>&1 || true
  mkdir -p "/storage/$TAG"

  {
    echo "# $TAG — EXACT launch command (durable on-rig copy)"
    echo "# Launched $(date -u +%Y-%m-%dT%H:%M:%SZ) on claw2 GB10 by retrain_queue_cs_aug4.sh"
    echo "# Corpus: data_cs_aug4 (dataset 2026-07-15-1, aether 2026-07-09-1, post setTypeValue fix)"
    echo
    echo "docker run -d --name $C --gpus all --ipc=host --shm-size=16g \\"
    echo "  --ulimit memlock=-1 --ulimit stack=67108864 \\"
    echo "  -v $W:/workspace -v /storage:/storage -v /slows:/slows -e HF_HOME=/storage/hf \\"
    echo "  $IMG \\"
    echo "  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \\"
    echo "    --model-id $MODEL \\"
    echo "    --instruction-jsonl /workspace/$DATA/aether_instruction_sft.jsonl \\"
    echo "    --repair-jsonl /workspace/$DATA/aether_repair_sft.jsonl \\"
    echo "    --output-dir /workspace/runs/sft-$TAG \\"
    [ "$FOURBIT" = "0" ] && echo "    --no-load-in-4bit \\"
    echo "    --epochs $EP --lora-r $R --lora-alpha $ALPHA \\"
    echo "    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \\"
    if [ -n "$IPART" ]; then echo "    --instruction-part \"$IPART\" --response-part \"$RPART\" \\"; fi
    echo "    --export-merged-16bit --merged-output-dir $MERGED"
  } > "/storage/$TAG/LAUNCH_COMMAND.md"

  log "LAUNCH $TAG container=$C"
  docker run -d --name "$C" --gpus all --ipc=host --shm-size=16g \
    --ulimit memlock=-1 --ulimit stack=67108864 \
    -v "$W":/workspace -v /storage:/storage -v /slows:/slows -e HF_HOME=/storage/hf \
    "$IMG" \
    python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
      --model-id "$MODEL" \
      --instruction-jsonl "/workspace/$DATA/aether_instruction_sft.jsonl" \
      --repair-jsonl "/workspace/$DATA/aether_repair_sft.jsonl" \
      --output-dir "/workspace/runs/sft-$TAG" \
      ${QBIT[@]+"${QBIT[@]}"} --epochs "$EP" --lora-r "$R" --lora-alpha "$ALPHA" \
      --learning-rate 1e-4 --batch-size 1 --grad-accum 8 ${EXTRA[@]+"${EXTRA[@]}"} \
      --export-merged-16bit --merged-output-dir "$MERGED" \
      > "$QDIR/$TAG.cid" 2>"$QDIR/$TAG.launch.err"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    log "FAIL $TAG — docker run returned $rc; err: $(tail -3 "$QDIR/$TAG.launch.err" 2>/dev/null)"
    continue
  fi

  log "WAIT $TAG — blocking until container exits"
  WAIT_RC="$(docker wait "$C" 2>/dev/null)"
  log "DONE $TAG — container exit code=$WAIT_RC"
  docker logs "$C" > "$QDIR/$TAG.log" 2>&1 || true

  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    nsh="$(ls "$MERGED"/*.safetensors 2>/dev/null | wc -l)"
    log "OK $TAG — merged export present ($nsh shards) at $MERGED"
  else
    log "WARN $TAG — container exited ($WAIT_RC) but NO merged export at $MERGED; check $QDIR/$TAG.log"
    if grep -qiE "Removed all|num_samples" "$QDIR/$TAG.log" 2>/dev/null; then
      log "DIAG $TAG — marker masking collapse fingerprint"
    fi
  fi
done

log "QUEUE COMPLETE"
touch "$QDIR/queue.done"
