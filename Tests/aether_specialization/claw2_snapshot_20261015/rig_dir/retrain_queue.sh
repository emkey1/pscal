#!/bin/bash
# Sequential Aether retrain queue on the single GB10 GPU.
# Each model trains to completion + merges, THEN the next starts (one GPU).
# Resumable: skips a model whose merged export already exists.
# Detached: launched via setsid nohup; survives SSH drops + orchestrator session death.
# set -u (catch unset), NOT set -e (a failed model logs and the queue continues).
set -u

W=$HOME/training/aether-qwen-coder-30b-unsloth
DATA=data_cs_aug2_builtins
QDIR=/storage/aether_retrain_queue
QLOG="$QDIR/queue.log"
mkdir -p "$QDIR"

log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QLOG"; }

# Queue rows: tag | base-model-path | r | alpha | epochs | INSTR_PART | RESP_PART
# Empty INSTR/RESP = use the trainer ChatML defaults (Qwen3 family).
# Order: smaller/safer dense + the proven Qwen3MoE first; q36 is NOT in this queue (held for owner go-ahead).
QUEUE=(
"a3b-coder30b-cs-aug2-builtins|/storage/archive/qwen3-coder-30b-a3b-model-mirror|32|64|3||"
"deepseek6.7b-cs-aug2-builtins|/storage/hf/hub/models--deepseek-ai--deepseek-coder-6.7b-instruct/snapshots/e5d64addd26a6a1db0f9b863abf6ee3141936807|32|64|3|### Instruction:|### Response:"
"qwen14b-cs-aug2-builtins|/storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671/|64|128|4||"
"qwen3-8b-nothink-cs-aug2-builtins|/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218/|32|64|3||"
)

log "QUEUE START — ${#QUEUE[@]} models, data=$DATA"

for row in "${QUEUE[@]}"; do
  IFS='|' read -r TAG MODEL R ALPHA EP IPART RPART <<< "$row"
  C="aether-train-$TAG"
  MERGED="/storage/$TAG/merged_16bit"

  log "=== model: $TAG (r=$R a=$ALPHA ep=$EP) ==="

  # Resumable skip: merged export already present + non-trivial
  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    log "SKIP $TAG — merged export already exists ($MERGED)"
    continue
  fi

  # Base present?
  if [ ! -f "$MODEL/config.json" ]; then
    log "FAIL $TAG — base model missing: $MODEL (skipping)"
    continue
  fi

  # Build EXTRA marker args
  EXTRA=()
  if [ -n "$IPART" ]; then EXTRA+=(--instruction-part "$IPART" --response-part "$RPART"); fi

  # Clear any stale container of this name
  docker rm -f "$C" >/dev/null 2>&1 || true
  mkdir -p "/storage/$TAG"

  # DOCUMENT at launch (durable on-rig copy) BEFORE starting
  {
    echo "# $TAG — EXACT launch command (durable on-rig copy)"
    echo "# Launched $(date -u +%Y-%m-%dT%H:%M:%SZ) on claw2 GB10 by retrain_queue.sh"
    echo "# Mirror: PBuild Tests/aether_specialization/training_runs/$TAG.md"
    echo
    echo "docker run -d --name $C --gpus all --ipc=host --shm-size=16g \\"
    echo "  --ulimit memlock=-1 --ulimit stack=67108864 \\"
    echo "  -v $W:/workspace -v /storage:/storage -e HF_HOME=/storage/hf \\"
    echo "  aether-unsloth-qwen3-coder-30b:q3 \\"
    echo "  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \\"
    echo "    --model-id $MODEL \\"
    echo "    --instruction-jsonl /workspace/$DATA/aether_instruction_sft.jsonl \\"
    echo "    --repair-jsonl /workspace/$DATA/aether_repair_sft.jsonl \\"
    echo "    --output-dir /workspace/runs/sft-$TAG \\"
    echo "    --no-load-in-4bit --epochs $EP --lora-r $R --lora-alpha $ALPHA \\"
    echo "    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \\"
    if [ -n "$IPART" ]; then echo "    --instruction-part '$IPART' --response-part '$RPART' \\"; fi
    echo "    --export-merged-16bit --merged-output-dir $MERGED"
    echo
    echo "# Base arch / markers: see training_runs/$TAG.md"
  } > "/storage/$TAG/LAUNCH_COMMAND.md"

  # LAUNCH (docker -d; train_any.sh-equivalent inlined so we control wait)
  log "LAUNCH $TAG container=$C"
  docker run -d --name "$C" --gpus all --ipc=host --shm-size=16g \
    --ulimit memlock=-1 --ulimit stack=67108864 \
    -v "$W":/workspace -v /storage:/storage -e HF_HOME=/storage/hf \
    aether-unsloth-qwen3-coder-30b:q3 \
    python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
      --model-id "$MODEL" \
      --instruction-jsonl "/workspace/$DATA/aether_instruction_sft.jsonl" \
      --repair-jsonl "/workspace/$DATA/aether_repair_sft.jsonl" \
      --output-dir "/workspace/runs/sft-$TAG" \
      --no-load-in-4bit --epochs "$EP" --lora-r "$R" --lora-alpha "$ALPHA" \
      --learning-rate 1e-4 --batch-size 1 --grad-accum 8 "${EXTRA[@]+"${EXTRA[@]}"}" \
      --export-merged-16bit --merged-output-dir "$MERGED" \
      > "$QDIR/$TAG.cid" 2>"$QDIR/$TAG.launch.err"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    log "FAIL $TAG — docker run returned $rc; err: $(cat "$QDIR/$TAG.launch.err" 2>/dev/null | tail -3)"
    continue
  fi

  # Wait for THIS container to finish before the next (sequential on one GPU)
  log "WAIT $TAG — blocking until container exits"
  WAIT_RC="$(docker wait "$C" 2>/dev/null)"
  log "DONE $TAG — container exit code=$WAIT_RC"
  # Capture full log durably
  docker logs "$C" > "$QDIR/$TAG.log" 2>&1 || true

  # Verify merge
  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    nsh="$(ls "$MERGED"/*.safetensors 2>/dev/null | wc -l)"
    log "OK $TAG — merged export present ($nsh shards) at $MERGED"
  else
    log "WARN $TAG — container exited ($WAIT_RC) but NO merged export at $MERGED; check $QDIR/$TAG.log"
    # masking-collapse fingerprint
    if grep -qiE "Removed all|num_samples" "$QDIR/$TAG.log" 2>/dev/null; then
      log "DIAG $TAG — marker masking collapse (Removed all / num_samples). Markers wrong for this template."
    fi
  fi
done

log "QUEUE COMPLETE"
touch "$QDIR/queue.done"
