#!/bin/bash
# Sequential cs-aug20 retrain queue for the LARGER model roster on claw2 GB10.
# Companion to retrain_queue_cs_aug20_precision.sh, which covered only the
# 8B/9B tier; this fills in the 14B-35B tier that had never been trained on the
# 0-based corpus.
#
# Precision: 4-bit for every model. Rationale: (a) it fits -- CUDA reports
# ~31.8 GiB free on this box, so 8-bit at 27B+ (~27 GB of weights before
# activations) is not viable, even though 8-bit beat 4-bit on the 8B/9B tier in
# cs-aug18/19/20; (b) most cs-aug4 big-model runs were 4-bit, so 4-bit-vs-4-bit
# is the controlled corpus comparison against that board.
#
# Images and base paths are taken from each model's own cs-aug3/cs-aug4
# LAUNCH_COMMAND.md rather than guessed -- the qwen36 family needs base26-fla,
# the rest use q3, and a wrong image is a dead run.
#
# Resumable (skips models whose merged export exists); detached; set -u not -e.
set -u

W=$HOME/training/aether-qwen-coder-30b-unsloth
DATA=data_cs_aug20
QDIR=/storage/aether_retrain_queue_cs_aug20_large
QLOG="$QDIR/queue.log"
mkdir -p "$QDIR"

log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$QLOG"; }

Q25_14B=/storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671
MISTRAL24B=/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724
Q36_27B=/storage/models/Qwen3.6-27B
Q3_CODER30B=/slows/archive/qwen3-coder-30b-a3b-model-mirror
Q36_35B_A3B=/storage/models/Qwen3.6-35B-A3B-Instruct

# smallest first: banks results early and surfaces any corpus/image problem on
# the cheapest run rather than 3 hours into the 35B.
# tag | image | base-model-path | precision | r | alpha | epochs
QUEUE=(
"qwen25-14b-cs-aug20|aether-unsloth-qwen3-coder-30b:q3|$Q25_14B|4bit|32|64|3"
"mistral24b-cs-aug20|aether-unsloth-qwen3-coder-30b:q3|$MISTRAL24B|4bit|32|64|3"
"qwen36-27b-cs-aug20|aether-unsloth-qwen3-coder-30b:base26-fla|$Q36_27B|4bit|32|64|3"
"qwen3-coder30b-a3b-cs-aug20|aether-unsloth-qwen3-coder-30b:q3|$Q3_CODER30B|4bit|32|64|3"
"qwen36-35b-a3b-cs-aug20|aether-unsloth-qwen3-coder-30b:base26-fla|$Q36_35B_A3B|4bit|32|64|3"
)

log "QUEUE START - ${#QUEUE[@]} runs, data=$DATA (dataset 2026-07-25-1, 726 instruction / 38 repair records), all 4-bit"
log "free VRAM before start: $(docker run --rm --gpus all --entrypoint python3 vllm/vllm-openai:latest -c 'import torch;f,t=torch.cuda.mem_get_info();print(f\"{f/2**30:.1f} GiB free of {t/2**30:.1f}\")' 2>/dev/null | tail -1)"

for row in "${QUEUE[@]}"; do
  IFS="|" read -r TAG IMG MODEL PREC R ALPHA EP <<< "$row"
  C="aether-train-$TAG"
  MERGED="/storage/$TAG/merged_16bit"

  log "=== model: $TAG (img=$IMG r=$R a=$ALPHA ep=$EP precision=$PREC) ==="

  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    log "SKIP $TAG - merged export already exists ($MERGED)"
    continue
  fi
  if [ ! -f "$MODEL/config.json" ]; then
    log "FAIL $TAG - base model missing: $MODEL (skipping)"
    continue
  fi
  if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    log "FAIL $TAG - docker image missing: $IMG (skipping)"
    continue
  fi

  QBIT=()
  case "$PREC" in
    4bit)  ;; # script default is --load-in-4bit
    8bit)  QBIT+=(--load-in-8bit --no-load-in-4bit) ;;
    16bit) QBIT+=(--no-load-in-4bit) ;;
    *) log "FAIL $TAG - unknown precision $PREC (skipping)"; continue ;;
  esac

  docker rm -f "$C" >/dev/null 2>&1 || true
  mkdir -p "/storage/$TAG"

  {
    echo "# $TAG - EXACT launch command (durable on-rig copy)"
    echo "# Launched $(date -u +%Y-%m-%dT%H:%M:%SZ) on claw2 GB10 by retrain_queue_cs_aug20_large.sh"
    echo "# Corpus: data_cs_aug20 (dataset 2026-07-25-1, 726 instruction / 38 repair records)"
    echo "# Precision: $PREC"
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
    for f in "${QBIT[@]+"${QBIT[@]}"}"; do echo "    $f \\"; done
    echo "    --epochs $EP --lora-r $R --lora-alpha $ALPHA \\"
    echo "    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \\"
    echo "    --export-merged-16bit --merged-output-dir $MERGED"
  } > "/storage/$TAG/LAUNCH_COMMAND.md"

  log "LAUNCH $TAG container=$C"
  # /slows mounted too: qwen3-coder30b-a3b's base weights live there, not /storage
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
      --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
      --export-merged-16bit --merged-output-dir "$MERGED" \
      > "$QDIR/$TAG.cid" 2>"$QDIR/$TAG.launch.err"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    log "FAIL $TAG - docker run returned $rc; err: $(tail -3 "$QDIR/$TAG.launch.err" 2>/dev/null)"
    continue
  fi

  log "WAIT $TAG - blocking until container exits"
  WAIT_RC="$(docker wait "$C" 2>/dev/null)"
  log "DONE $TAG - container exit code=$WAIT_RC"
  docker logs "$C" > "$QDIR/$TAG.log" 2>&1 || true

  if [ -f "$MERGED/config.json" ] && ls "$MERGED"/*.safetensors >/dev/null 2>&1; then
    nsh="$(ls "$MERGED"/*.safetensors 2>/dev/null | wc -l)"
    sz="$(du -sh "$MERGED" 2>/dev/null | cut -f1)"
    log "OK $TAG - merged export present ($nsh shards, $sz) at $MERGED"
  else
    log "WARN $TAG - container exited ($WAIT_RC) but NO merged export at $MERGED; check $QDIR/$TAG.log"
    if grep -qiE "out of memory|OutOfMemory|CUDA error" "$QDIR/$TAG.log" 2>/dev/null; then
      log "DIAG $TAG - OOM fingerprint; 4-bit did not fit, needs a smaller footprint"
    fi
    if grep -qiE "Removed all|num_samples" "$QDIR/$TAG.log" 2>/dev/null; then
      log "DIAG $TAG - marker masking collapse fingerprint"
    fi
  fi
  docker rm -f "$C" >/dev/null 2>&1 || true
done

log "QUEUE COMPLETE"
touch "$QDIR/queue.done"
