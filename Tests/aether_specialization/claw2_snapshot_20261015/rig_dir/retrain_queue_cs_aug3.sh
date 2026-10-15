#!/bin/bash
# Sequential Aether cs_aug3 retrain queue on the single GB10 GPU (clone of the
# proven retrain_queue.sh, data=data_cs_aug3, aether 2026-07-01-8 corpus).
# Resumable (skips models whose merged export exists); detached; set -u not -e.
set -u

W=$HOME/training/aether-qwen-coder-30b-unsloth
DATA=data_cs_aug3
QDIR=/storage/aether_retrain_queue_cs_aug3
QLOG="$QDIR/queue.log"
mkdir -p "$QDIR"

log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QLOG"; }

# tag | base-model-path | r | alpha | epochs | INSTR_PART | RESP_PART
A3B_BASE=/slows/archive/qwen3-coder-30b-a3b-model-mirror
[ -f "$A3B_BASE/config.json" ] || A3B_BASE=$(ls -d /storage/hf/hub/models--Qwen--Qwen3-Coder-30B-A3B-Instruct/snapshots/*/ 2>/dev/null | head -1)

QUEUE=(
"a3b-coder30b-cs-aug3|${A3B_BASE:-/nonexistent}|32|64|3||"
"mistral24b-cs-aug3|/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724/|64|128|4|[INST]|[/INST]"
"qwen14b-cs-aug3|/storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671/|64|128|4||"
"qwen3-8b-nothink-cs-aug3|/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218/|32|64|3||"
)

log "QUEUE START — ${#QUEUE[@]} models, data=$DATA (corpus 2026-07-01-1 / aether 2026-07-01-8)"

for row in "${QUEUE[@]}"; do
  IFS="|" read -r TAG MODEL R ALPHA EP IPART RPART <<< "$row"
  C="aether-train-$TAG"
  MERGED="/storage/$TAG/merged_16bit"

  log "=== model: $TAG (r=$R a=$ALPHA ep=$EP) ==="

  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    log "SKIP $TAG — merged export already exists ($MERGED)"
    continue
  fi
  if [ ! -f "$MODEL/config.json" ]; then
    log "FAIL $TAG — base model missing: $MODEL (skipping)"
    continue
  fi

  EXTRA=()
  if [ -n "$IPART" ]; then EXTRA+=(--instruction-part "$IPART" --response-part "$RPART"); fi

  docker rm -f "$C" >/dev/null 2>&1 || true
  mkdir -p "/storage/$TAG"

  {
    echo "# $TAG — EXACT launch command (durable on-rig copy)"
    echo "# Launched $(date -u +%Y-%m-%dT%H:%M:%SZ) on claw2 GB10 by retrain_queue_cs_aug3.sh"
    echo "# Corpus: data_cs_aug3 (dataset 2026-07-01-1, aether 2026-07-01-8)"
    echo
    echo "docker run -d --name $C --gpus all --ipc=host --shm-size=16g \\"
    echo "  --ulimit memlock=-1 --ulimit stack=67108864 \\"
    echo "  -v $W:/workspace -v /storage:/storage -v /slows:/slows -e HF_HOME=/storage/hf \\"
    echo "  aether-unsloth-qwen3-coder-30b:q3 \\"
    echo "  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \\"
    echo "    --model-id $MODEL \\"
    echo "    --instruction-jsonl /workspace/$DATA/aether_instruction_sft.jsonl \\"
    echo "    --repair-jsonl /workspace/$DATA/aether_repair_sft.jsonl \\"
    echo "    --output-dir /workspace/runs/sft-$TAG \\"
    echo "    --no-load-in-4bit --epochs $EP --lora-r $R --lora-alpha $ALPHA \\"
    echo "    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \\"
    if [ -n "$IPART" ]; then echo "    --instruction-part \"$IPART\" --response-part \"$RPART\" \\"; fi
    echo "    --export-merged-16bit --merged-output-dir $MERGED"
  } > "/storage/$TAG/LAUNCH_COMMAND.md"

  log "LAUNCH $TAG container=$C"
  docker run -d --name "$C" --gpus all --ipc=host --shm-size=16g \
    --ulimit memlock=-1 --ulimit stack=67108864 \
    -v "$W":/workspace -v /storage:/storage -v /slows:/slows -e HF_HOME=/storage/hf \
    aether-unsloth-qwen3-coder-30b:q3 \
    python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
      --model-id "$MODEL" \
      --instruction-jsonl "/workspace/$DATA/aether_instruction_sft.jsonl" \
      --repair-jsonl "/workspace/$DATA/aether_repair_sft.jsonl" \
      --output-dir "/workspace/runs/sft-$TAG" \
      --no-load-in-4bit --epochs "$EP" --lora-r "$R" --lora-alpha "$ALPHA" \
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
