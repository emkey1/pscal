#!/bin/bash
# Early-stopping experiment: is the current criterion stopping on noise?
#
# CONTROL is the existing qwen3-8b-nothink-8bit-cs-aug20 run (already on disk):
#   eval-cases 12, eval-steps 5, patience 2, threshold 0.0  -> stopped at epoch 1.70
#
# TREATMENT (this run) makes eval_loss actually resolvable before trusting it:
#   eval-cases 75    a 12-sample loss cannot resolve the ~0.1% changes the old
#                    config was stopping on. 75 is ~10% of the 764-record pool.
#   eval-steps 20    less twitchy, and cheaper than evaluating every 5 steps
#   patience 4       4 consecutive non-improvements, not 2
#   threshold 0.001  an uptick must exceed 0.1% to count; the stop that ended the
#                    16-bit run was +0.00006 (0.03%)
#
# Same model, data, LoRA rank, lr, batch and grad-accum as the control. One
# confound worth naming: a 75-case holdout leaves ~689 training records instead
# of 752 (~8% less), so a score gain is "better stopping despite less data" and
# a small loss is not necessarily a refutation. The stopping EPOCH is the cleaner
# signal here; the benchmark score is secondary.
set -u

W=$HOME/training/aether-qwen-coder-30b-unsloth
DATA=data_cs_aug20
TAG=qwen3-8b-nothink-8bit-cs-aug20-es2
IMG=aether-unsloth-qwen3-coder-30b:q3
MODEL=/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218
MERGED="/storage/$TAG/merged_16bit"
QDIR=/storage/aether_es_experiment
C="aether-train-$TAG"
mkdir -p "$QDIR"
log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QDIR/queue.log"; }

if [ -f "$MERGED/config.json" ]; then log "SKIP $TAG - merged export exists"; exit 0; fi

log "=== $TAG: eval-cases 75, eval-steps 20, patience 4, threshold 0.001 ==="
log "control for comparison: qwen3-8b-nothink-8bit-cs-aug20 stopped at epoch 1.70"
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
    --load-in-8bit --no-load-in-4bit \
    --eval-cases 75 --eval-steps 20 --save-steps 20 \
    --early-stopping-patience 4 --early-stopping-threshold 0.001 \
    --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir "$MERGED" \
    > "$QDIR/$TAG.cid" 2>"$QDIR/$TAG.launch.err"
rc=$?
[ "$rc" -ne 0 ] && { log "FAIL - docker run rc=$rc: $(tail -3 "$QDIR/$TAG.launch.err")"; exit 1; }

# fail fast if the flags were rejected or masking collapsed
sleep 180
if docker logs "$C" 2>&1 | grep -qE "unrecognized arguments|num_samples=0|Removed [0-9]+ out of \1"; then
  log "FAIL - bad flags or masking collapse:"
  docker logs "$C" 2>&1 | grep -E "unrecognized arguments|Removed .* out of|ValueError" | tail -3 | tee -a "$QDIR/queue.log"
  docker rm -f "$C" >/dev/null 2>&1; exit 1
fi
log "launched OK: $(docker logs "$C" 2>&1 | grep -oE 'Num examples = [0-9]+ \| Num Epochs = [0-9]+ \| Total steps = [0-9]+' | head -1)"

log "WAIT - blocking until container exits"
WAIT_RC="$(docker wait "$C" 2>/dev/null)"
log "DONE - exit=$WAIT_RC"
docker logs "$C" > "$QDIR/$TAG.log" 2>&1 || true

ep="$(grep -oE "[\"']epoch[\"']:\s*[\"']?[0-9.]+" "$QDIR/$TAG.log" | tail -1 | grep -oE '[0-9.]+$')"
log "STOPPED AT EPOCH: ${ep:-unknown}   (control stopped at 1.70)"
if [ -f "$MERGED/config.json" ]; then
  log "OK - merged export present ($(du -sh "$MERGED" | cut -f1))"
else
  log "WARN - no merged export; check $QDIR/$TAG.log"
fi
docker rm -f "$C" >/dev/null 2>&1 || true
touch "$QDIR/es_experiment.done"
