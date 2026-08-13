# Training run: qwen14b-cs-aug2-builtins

Qwen2.5-Coder-14B-Instruct (DENSE, `Qwen2ForCausalLM`) Aether fine-tune on the
`out_cs_aug2_builtins` corpus. Re-run of the prior `qwen14b-ml3x` recipe
(`runs/sft-qwen14b-ml3x/run_metadata.json`), data dir swapped to `data_cs_aug2_builtins`.

- **Launched:** 2026-06-28 (via `retrain_queue.sh`, queue position 3)
- **Rig:** claw2 GB10 · container `aether-train-qwen14b-cs-aug2-builtins`
- **Base:** `/storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671/` (arch `Qwen2ForCausalLM`, present, 6 shards)
- **Markers:** ChatML — trainer defaults `<|im_start|>user\n` / `<|im_start|>assistant\n`
  (probe-confirmed; NO override).
- **Recipe:** r=64, alpha=128, epochs=4, lr=1e-4, bs=1, ga=8, `--no-load-in-4bit`
- **Export → `/storage/qwen14b-cs-aug2-builtins/merged_16bit`**

## Exact docker run

```bash
docker run -d --name aether-train-qwen14b-cs-aug2-builtins \
  --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v /home/claw/training/aether-qwen-coder-30b-unsloth:/workspace \
  -v /storage:/storage -e HF_HOME=/storage/hf \
  aether-unsloth-qwen3-coder-30b:q3 \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671/ \
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-qwen14b-cs-aug2-builtins \
    --no-load-in-4bit --epochs 4 --lora-r 64 --lora-alpha 128 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir /storage/qwen14b-cs-aug2-builtins/merged_16bit
```

Equivalent wrapper: `./train_any.sh qwen14b-cs-aug2-builtins /storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671/ 64 128 data_cs_aug2_builtins 4`

## Check status
```bash
ssh claw@claw2.tailfe3968.ts.net \
  "docker ps -a --filter name=aether-train-qwen14b-cs-aug2-builtins --format '{{.Status}}'; \
   tail -30 /storage/aether_retrain_queue/qwen14b-cs-aug2-builtins.log 2>/dev/null; \
   ls /storage/qwen14b-cs-aug2-builtins/merged_16bit/ 2>/dev/null"
```
On-rig durable copy: `/storage/qwen14b-cs-aug2-builtins/LAUNCH_COMMAND.md`.
