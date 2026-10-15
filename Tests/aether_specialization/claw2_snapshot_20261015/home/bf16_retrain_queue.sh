#!/bin/bash
# Resumable bf16 retrain queue: everything <=27B moves from 4-bit QLoRA to full
# bf16 LoRA, matching qwen36-27b's existing precision. Skip-if-merged-exists,
# same recipe as the 4-bit runs (epochs 3, lora-r 32, lora-alpha 64, lr 1e-4,
# batch-size 1, grad-accum 8) so bf16 vs 4-bit is the only variable that changed.
#
# instruction-part/response-part default to ChatML (<|im_start|>user\n /
# <|im_start|>assistant\n) in the training script, which matches Qwen-family
# templates but NOT Mistral ([INST]...[/INST], no distinct response marker)
# or DeepSeek-Coder-Instruct (### Instruction:\n / ### Response:\n). Without
# the correct override, train_on_responses_only never finds the response
# span and silently masks 100% of training samples to -100 (num_samples=0
# crash) -- discovered when both deepseek6.7b and mistral24b hit this
# identically on the first pass; the 3 Qwen-family models never needed it.
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth

# tag | model_path | docker_image | instruction_part | response_part
MODELS=(
 "deepseek6.7b-cs-aug4|/storage/hf/hub/models--deepseek-ai--deepseek-coder-6.7b-instruct/snapshots/e5d64addd26a6a1db0f9b863abf6ee3141936807|q3|### Instruction:\n|### Response:\n"
 "qwen3-8b-nothink-cs-aug4|/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218|q3||"
 "qwen35-9b-cs-aug4|/storage/models/Qwen3.5-9B|base26-fla||"
 "qwen25-14b-cs-aug4|/storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671|q3||"
 "mistral24b-cs-aug4|/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724|q3|[INST]|[/INST]"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r tag model image instr resp <<< "$entry"
  MERGED=/storage/$tag/merged_16bit
  if [ -d "$MERGED" ]; then
    echo "[$tag] merged export already exists, skip"
    continue
  fi
  echo "==== [$tag] START $(date) ===="
  mkdir -p /storage/$tag
  docker rm -f aether-train-$tag >/dev/null 2>&1
  extra_args=()
  [ -n "$instr" ] && extra_args+=(--instruction-part "$instr")
  [ -n "$resp" ] && extra_args+=(--response-part "$resp")
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
      --epochs 3 --lora-r 32 --lora-alpha 64 \
      --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
      "${extra_args[@]}" \
      --export-merged-16bit --merged-output-dir $MERGED
  rc=$?
  docker rm -f aether-train-$tag >/dev/null 2>&1
  if [ $rc -ne 0 ] || [ ! -d "$MERGED" ]; then
    echo "[$tag] FAILED rc=$rc, merged export missing — ABORTING QUEUE"
    exit 1
  fi
  echo "==== [$tag] DONE $(date) ===="
done
echo "==== BF16 RETRAIN QUEUE COMPLETE $(date) ===="
