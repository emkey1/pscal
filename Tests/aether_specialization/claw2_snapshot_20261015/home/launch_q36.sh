#!/bin/bash
# Launch q36 (Qwen3.6-35B-A3B hybrid qwen3_5_moe) retrain on the cs-aug2-builtins corpus.
# 4-bit QLoRA (q36's proven config; bf16 ~70GB would OOM 73G avail). Image base26-fla
# (the :q3 image cannot run qwen3_5_moe). SDPA hardcoded in the trainer script.
# Captures the EXACT docker run to /storage BEFORE launch — this command was lost once.
set -u
NAME=aether-train-q36-cs-aug2-builtins
OUT=/storage/q36-cs-aug2-builtins
WS=$HOME/training/aether-qwen-coder-30b-unsloth
IMG=aether-unsloth-qwen3-coder-30b:base26-fla
TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)

mkdir -p "$OUT"

cat > "$OUT/LAUNCH_COMMAND.md" <<EOF
# q36-cs-aug2-builtins — EXACT launch command (durable on-rig copy)
# Launched $TS on claw2 GB10 (Blackwell sm_121)
# Model: Qwen3.6-35B-A3B-Instruct (hybrid linear-attn + MoE, arch qwen3_5_moe)
# Quant: 4-bit QLoRA (proven q36 config; bf16 ~70GB would OOM 73G avail)
# Image: base26-fla (the :q3 image cannot run qwen3_5_moe). SDPA hardcoded in trainer.
# Corpus: data_cs_aug2_builtins (NEW — joins the 4 sibling cs-aug2-builtins retrains)
# Mirror: PBuild Tests/aether_specialization/training_runs/q36-aether-RECONSTRUCTED.md

docker run -d --name $NAME --gpus all --ipc=host --shm-size=16g \\
  --ulimit memlock=-1 --ulimit stack=67108864 \\
  -v $WS:/workspace -v /storage:/storage -e HF_HOME=/storage/hf \\
  $IMG \\
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \\
    --model-id /storage/models/Qwen3.6-35B-A3B-Instruct \\
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \\
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \\
    --output-dir /workspace/runs/sft-q36-cs-aug2-builtins \\
    --epochs 3 --lora-r 32 --lora-alpha 64 \\
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \\
    --export-merged-16bit --merged-output-dir $OUT/merged_16bit
EOF

docker rm -f "$NAME" >/dev/null 2>&1 || true

docker run -d --name "$NAME" --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v "$WS":/workspace -v /storage:/storage -e HF_HOME=/storage/hf \
  "$IMG" \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /storage/models/Qwen3.6-35B-A3B-Instruct \
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-q36-cs-aug2-builtins \
    --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir "$OUT/merged_16bit"

echo "LAUNCHED $NAME"
docker ps --filter "name=$NAME" --format "{{.Names}}  {{.Status}}  {{.ID}}"
echo "command captured -> $OUT/LAUNCH_COMMAND.md"
