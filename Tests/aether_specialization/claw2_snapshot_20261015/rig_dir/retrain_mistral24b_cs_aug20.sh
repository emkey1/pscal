#!/bin/bash
# Re-run mistral24b on cs-aug20 with Mistral's own response-masking markers.
#
# The main large-roster queue used the script defaults, which are ChatML/Qwen
# ("<|im_start|>user\n" / "<|im_start|>assistant\n"). Mistral-Small-24B renders
# as:
#   <s>[SYSTEM_PROMPT]...[/SYSTEM_PROMPT][INST]USERMSG[/INST]ASSISTANTMSG</s>
# so neither default marker appears, train_on_responses_only masked every token,
# and the run died with:
#   Removed 752 out of 752 samples ... where all labels were -100
#   ValueError: num_samples should be a positive integer value, but got 0
#
# Markers below are read off an actual rendered template, not guessed, and carry
# no trailing whitespace (Mistral puts none after [/INST], unlike ChatML's \n).
set -u

W=$HOME/training/aether-qwen-coder-30b-unsloth
DATA=data_cs_aug20
QDIR=/storage/aether_retrain_queue_cs_aug20_large
QLOG="$QDIR/queue.log"
mkdir -p "$QDIR"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QLOG"; }

TAG=mistral24b-cs-aug20
IMG=aether-unsloth-qwen3-coder-30b:q3
MODEL=/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724
MERGED="/storage/$TAG/merged_16bit"
C="aether-train-$TAG"
INSTR='[INST]'
RESP='[/INST]'

if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
  log "SKIP $TAG - merged export already exists"; exit 0
fi

log "=== RETRY $TAG with Mistral markers instruction='$INSTR' response='$RESP' ==="
docker rm -f "$C" >/dev/null 2>&1 || true
mkdir -p "/storage/$TAG"

docker run -d --name "$C" --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v "$W":/workspace -v /storage:/storage -e HF_HOME=/storage/hf \
  "$IMG" \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id "$MODEL" \
    --instruction-jsonl "/workspace/$DATA/aether_instruction_sft.jsonl" \
    --repair-jsonl "/workspace/$DATA/aether_repair_sft.jsonl" \
    --output-dir "/workspace/runs/sft-$TAG" \
    --instruction-part "$INSTR" --response-part "$RESP" \
    --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir "$MERGED" \
    > "$QDIR/$TAG.retry.cid" 2>"$QDIR/$TAG.retry.launch.err"
rc=$?
[ "$rc" -ne 0 ] && { log "FAIL $TAG - docker run rc=$rc: $(tail -3 "$QDIR/$TAG.retry.launch.err")"; exit 1; }

# fail fast: if masking still collapses it shows up within ~2 minutes, and there
# is no point holding the GPU for a run that has already lost all its labels.
sleep 150
if docker logs "$C" 2>&1 | grep -qE "Removed 752 out of 752|num_samples=0"; then
  log "FAIL $TAG - markers STILL collapse masking; aborting early"
  docker logs "$C" 2>&1 | grep -E "Removed .* out of|ValueError" | tail -3 | tee -a "$QLOG"
  docker rm -f "$C" >/dev/null 2>&1
  exit 1
fi
kept="$(docker logs "$C" 2>&1 | grep -oE "Removed [0-9]+ out of [0-9]+ samples" | head -1)"
log "$TAG masking OK ${kept:+($kept)}"

log "WAIT $TAG - blocking until container exits"
WAIT_RC="$(docker wait "$C" 2>/dev/null)"
log "DONE $TAG - exit=$WAIT_RC"
docker logs "$C" > "$QDIR/$TAG.log" 2>&1 || true

if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
  log "OK $TAG - merged export present ($(ls "$MERGED"/*.safetensors | wc -l) shards, $(du -sh "$MERGED" | cut -f1))"
else
  log "WARN $TAG - no merged export; check $QDIR/$TAG.log"
fi
docker rm -f "$C" >/dev/null 2>&1 || true
touch "$QDIR/mistral_retry.done"
