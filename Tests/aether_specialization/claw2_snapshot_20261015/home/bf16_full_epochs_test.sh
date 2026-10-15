#!/bin/bash
# Experiment: does forcing bf16 to train the FULL 3 epochs (165 steps), instead
# of stopping early (patience=2) once eval_loss on the 12-example holdout stops
# improving, recover the downstream benchmark regression seen vs 4-bit?
# Same corpus, same recipe. Two changes from the normal recipe, both required:
#  1. --early-stopping-patience 999 (effectively disabled -- 33 total eval
#     checkpoints at eval-every-5-steps, so 999 guarantees it never fires).
#  2. --no-load-best-model-at-end (new flag, patched into
#     unsloth_qwen_coder_30b_sft.py) -- without this, the trainer reloads
#     whatever checkpoint had the best eval_loss across the whole run before
#     exporting, which would just reproduce the same early checkpoint even if
#     training ran the full 165 steps. This exports the actual final-step
#     weights instead, so the extra training exposure is what actually ships.
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth

# tag | model_path | docker_image
MODELS=(
 "qwen3-8b-nothink-cs-aug4-fullep|/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218|q3"
 "qwen35-9b-cs-aug4-fullep|/storage/models/Qwen3.5-9B|base26-fla"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r tag model image <<< "$entry"
  MERGED=/storage/$tag/merged_16bit
  if [ -d "$MERGED" ]; then
    echo "[$tag] merged export already exists, skip"
    continue
  fi
  echo "==== [$tag] START $(date) ===="
  mkdir -p /storage/$tag
  docker rm -f aether-train-$tag >/dev/null 2>&1
  docker run --name aether-train-$tag --gpus all --ipc=host --shm-size=16g \
    --ulimit memlock=-1 --ulimit stack=67108864 \
    -v $W:/workspace -v /storage:/storage -e HF_HOME=/storage/hf \
    aether-unsloth-qwen3-coder-30b:$image \
    python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
      --model-id $model \
      --instruction-jsonl /workspace/data_cs_aug4/aether_instruction_sft.jsonl \
      --repair-jsonl /workspace/data_cs_aug4/aether_repair_sft.jsonl \
      --output-dir /workspace/runs/sft-$tag \
      --no-load-in-4bit \
      --early-stopping-patience 999 \
      --no-load-best-model-at-end \
      --epochs 3 --lora-r 32 --lora-alpha 64 \
      --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
      --export-merged-16bit --merged-output-dir $MERGED
  rc=$?
  docker rm -f aether-train-$tag >/dev/null 2>&1
  if [ $rc -ne 0 ] || [ ! -d "$MERGED" ]; then
    echo "[$tag] FAILED rc=$rc, merged export missing — ABORTING QUEUE"
    exit 1
  fi
  echo "==== [$tag] DONE $(date) ===="
done
echo "==== FULL-EPOCHS TEST QUEUE COMPLETE $(date) ===="
