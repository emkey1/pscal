#!/bin/bash
# Sequential Aether cs_aug18 precision-grid retrain queue on the single GB10 GPU
# (claw2). 2 models (qwen3-8b-nothink, qwen35-9b) x 3 precisions (4bit/8bit/16bit),
# same recipe as the cs-aug4 precision experiment (r=32 a=64 ep=3 lr=1e-4 bs=1 ga=8),
# on the current corpus (data_cs_aug18, aether_version 2026-07-19-9, 649 instruction /
# 38 repair records). Extends the bf16-vs-4bit precision experiment in
# aether_specialization_findings.md with the missing 8-bit point, via the new
# --load-in-8bit flag (bitsandbytes 8-bit, verified working for both qwen3 and
# qwen3_5 archs in this Unsloth 2026.6.6 install before this queue was written).
# Resumable (skips models whose merged export exists); detached; set -u not -e.
set -u

W=$HOME/training/aether-qwen-coder-30b-unsloth
DATA=data_cs_aug18
QDIR=/storage/aether_retrain_queue_cs_aug18_precision
QLOG="$QDIR/queue.log"
mkdir -p "$QDIR"

log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QLOG"; }

Q3_8B=/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218
Q35_9B=/storage/models/Qwen3.5-9B

# tag | image | base-model-path | precision(4bit/8bit/16bit) | r | alpha | epochs
QUEUE=(
"qwen3-8b-nothink-4bit-cs-aug18|aether-unsloth-qwen3-coder-30b:q3|$Q3_8B|4bit|32|64|3"
"qwen3-8b-nothink-8bit-cs-aug18|aether-unsloth-qwen3-coder-30b:q3|$Q3_8B|8bit|32|64|3"
"qwen3-8b-nothink-16bit-cs-aug18|aether-unsloth-qwen3-coder-30b:q3|$Q3_8B|16bit|32|64|3"
"qwen35-9b-4bit-cs-aug18|aether-unsloth-qwen3-coder-30b:tf520-fla|$Q35_9B|4bit|32|64|3"
"qwen35-9b-8bit-cs-aug18|aether-unsloth-qwen3-coder-30b:tf520-fla|$Q35_9B|8bit|32|64|3"
"qwen35-9b-16bit-cs-aug18|aether-unsloth-qwen3-coder-30b:tf520-fla|$Q35_9B|16bit|32|64|3"
)

log "QUEUE START — ${#QUEUE[@]} runs, data=$DATA (aether_version 2026-07-19-9, 649 instruction / 38 repair records)"

for row in "${QUEUE[@]}"; do
  IFS="|" read -r TAG IMG MODEL PREC R ALPHA EP <<< "$row"
  C="aether-train-$TAG"
  MERGED="/storage/$TAG/merged_16bit"

  log "=== model: $TAG (img=$IMG r=$R a=$ALPHA ep=$EP precision=$PREC) ==="

  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    log "SKIP $TAG — merged export already exists ($MERGED)"
    continue
  fi
  if [ ! -f "$MODEL/config.json" ]; then
    log "FAIL $TAG — base model missing: $MODEL (skipping)"
    continue
  fi

  QBIT=()
  case "$PREC" in
    4bit)  ;; # script default is --load-in-4bit
    8bit)  QBIT+=(--load-in-8bit --no-load-in-4bit) ;;
    16bit) QBIT+=(--no-load-in-4bit) ;;
    *) log "FAIL $TAG — unknown precision $PREC (skipping)"; continue ;;
  esac

  docker rm -f "$C" >/dev/null 2>&1 || true
  mkdir -p "/storage/$TAG"

  {
    echo "# $TAG — EXACT launch command (durable on-rig copy)"
    echo "# Launched $(date -u +%Y-%m-%dT%H:%M:%SZ) on claw2 GB10 by retrain_queue_cs_aug18_precision.sh"
    echo "# Corpus: data_cs_aug18 (aether_version 2026-07-19-9, 649 instruction / 38 repair records)"
    echo "# Precision: $PREC"
    echo
    echo "docker run -d --name $C --gpus all --ipc=host --shm-size=16g \\"
    echo "  --ulimit memlock=-1 --ulimit stack=67108864 \\"
    echo "  -v $W:/workspace -v /storage:/storage -e HF_HOME=/storage/hf \\"
    echo "  $IMG \\"
    echo "  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \\"
    echo "    --model-id $MODEL \\"
    echo "    --instruction-jsonl /workspace/$DATA/aether_instruction_sft.jsonl \\"
    echo "    --repair-jsonl /workspace/$DATA/aether_repair_sft.jsonl \\"
    echo "    --output-dir /workspace/runs/sft-$TAG \\"
    for f in "${QBIT[@]+"${QBIT[@]}"}"; do echo "    $f \\"; done
    echo "    --epochs $EP --lora-r $R --lora-alpha $ALPHA \\"
    echo "    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \\"
    echo "    --export-merged-16bit --merged-output-dir $MERGED"
  } > "/storage/$TAG/LAUNCH_COMMAND.md"

  log "LAUNCH $TAG container=$C"
  docker run -d --name "$C" --gpus all --ipc=host --shm-size=16g \
    --ulimit memlock=-1 --ulimit stack=67108864 \
    -v "$W":/workspace -v /storage:/storage -e HF_HOME=/storage/hf \
    "$IMG" \
    python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
      --model-id "$MODEL" \
      --instruction-jsonl "/workspace/$DATA/aether_instruction_sft.jsonl" \
      --repair-jsonl "/workspace/$DATA/aether_repair_sft.jsonl" \
      --output-dir "/workspace/runs/sft-$TAG" \
      ${QBIT[@]+"${QBIT[@]}"} --epochs "$EP" --lora-r "$R" --lora-alpha "$ALPHA" \
      --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
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
