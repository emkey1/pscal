# Training run: qwen3-8b-nothink-cs-aug2-builtins

Qwen3-8B (DENSE, `Qwen3ForCausalLM`), NON-thinking recipe, Aether fine-tune on the
`out_cs_aug2_builtins` corpus. Re-run of the prior `qwen3-8b-nothink` recipe
(`runs/sft-qwen3-8b-nothink/run_metadata.json`), data dir swapped to `data_cs_aug2_builtins`.

- **Launched:** 2026-06-28 (via `retrain_queue.sh`, queue position 4)
- **Rig:** claw2 GB10 · container `aether-train-qwen3-8b-nothink-cs-aug2-builtins`
- **Base:** `/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218/` (arch `Qwen3ForCausalLM`, present, 5 shards)
- **NOTHINK rationale:** the new corpus is standard instruction data (plain Aether
  responses, no reasoning traces), so the non-thinking recipe is correct. Project history
  (aether-thinking-training memory): thinking-SFT on synthetic traces HURT the no-guide
  KPI at every corpus size (taught form not function); nothink is the right default here.
- **Markers:** ChatML — trainer defaults `<|im_start|>user\n` / `<|im_start|>assistant\n`.
  Qwen3's template injects an empty `<think>\n\n</think>` block after the assistant marker;
  `train_on_responses_only` matches `<|im_start|>assistant\n` as the response boundary.
  The queue verifies `Filter N/N` is non-zero post-launch (catches any masking surprise
  from the think injection); if it collapses, that's the signal to REPORT, not thrash.
- **Recipe:** r=32, alpha=64, epochs=3, lr=1e-4, bs=1, ga=8, `--no-load-in-4bit`
- **Export → `/storage/qwen3-8b-nothink-cs-aug2-builtins/merged_16bit`**

## Exact docker run

```bash
docker run -d --name aether-train-qwen3-8b-nothink-cs-aug2-builtins \
  --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v /home/claw/training/aether-qwen-coder-30b-unsloth:/workspace \
  -v /storage:/storage -e HF_HOME=/storage/hf \
  aether-unsloth-qwen3-coder-30b:q3 \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218/ \
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-qwen3-8b-nothink-cs-aug2-builtins \
    --no-load-in-4bit --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir /storage/qwen3-8b-nothink-cs-aug2-builtins/merged_16bit
```

Equivalent wrapper: `./train_any.sh qwen3-8b-nothink-cs-aug2-builtins /storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218/ 32 64 data_cs_aug2_builtins 3`

## Check status
```bash
ssh claw@claw2.tailfe3968.ts.net \
  "docker ps -a --filter name=aether-train-qwen3-8b-nothink-cs-aug2-builtins --format '{{.Status}}'; \
   tail -30 /storage/aether_retrain_queue/qwen3-8b-nothink-cs-aug2-builtins.log 2>/dev/null; \
   ls /storage/qwen3-8b-nothink-cs-aug2-builtins/merged_16bit/ 2>/dev/null"
```
On-rig durable copy: `/storage/qwen3-8b-nothink-cs-aug2-builtins/LAUNCH_COMMAND.md`.
