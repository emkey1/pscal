# Training run: deepseek6.7b-cs-aug2-builtins

DeepSeek-Coder-6.7B-Instruct (DENSE, `LlamaForCausalLM`) Aether fine-tune on the
`out_cs_aug2_builtins` corpus. Re-run of the prior `deepseek-ml1x` recipe
(`runs/sft-deepseek-ml1x/run_metadata.json`), data dir swapped to `data_cs_aug2_builtins`.

> **NOTE on identity:** the expanded-scope request labeled this slot "DeepSeek-V2-Lite
> (MoE)", but the recipe it pointed to (`runs/sft-deepseek-ml1x`) is
> **deepseek-coder-6.7b-instruct — a DENSE Llama-arch model**, NOT V2-Lite. Per the
> "keep the base from the established recipe" rule, this run uses the 6.7B dense base
> that the metadata specifies (also simpler/safer: dense Llama avoids the Blackwell MoE
> walls entirely). If a true DeepSeek-V2-Lite MoE retrain is wanted, that is a different
> base + recipe — flag for the owner.

- **Launched:** 2026-06-28 (via `retrain_queue.sh`, queue position 2)
- **Rig:** claw2 GB10 · container `aether-train-deepseek6.7b-cs-aug2-builtins`
- **Base:** `/storage/hf/hub/models--deepseek-ai--deepseek-coder-6.7b-instruct/snapshots/e5d64addd26a6a1db0f9b863abf6ee3141936807` (arch `LlamaForCausalLM`, present, 2 shards)
- **Markers — Alpaca, NOT ChatML (probe-confirmed):** `INSTR_PART="### Instruction:"`
  `RESP_PART="### Response:"`. The rendered template is `### Instruction:\n…\n### Response:\n…`.
  WRONG markers here = total masking collapse (`Removed all samples → num_samples=0`).
  The queue verifies the `Filter N/N` is non-zero post-launch; if it collapses, retry
  with `RESP_PART="### Response:\n"` (no-trailing-space fuse rule).
- **Recipe:** r=32, alpha=64, epochs=3, lr=1e-4, bs=1, ga=8, `--no-load-in-4bit`
- **Export → `/storage/deepseek6.7b-cs-aug2-builtins/merged_16bit`**

## Exact docker run

```bash
docker run -d --name aether-train-deepseek6.7b-cs-aug2-builtins \
  --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v /home/claw/training/aether-qwen-coder-30b-unsloth:/workspace \
  -v /storage:/storage -e HF_HOME=/storage/hf \
  aether-unsloth-qwen3-coder-30b:q3 \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /storage/hf/hub/models--deepseek-ai--deepseek-coder-6.7b-instruct/snapshots/e5d64addd26a6a1db0f9b863abf6ee3141936807 \
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-deepseek6.7b-cs-aug2-builtins \
    --no-load-in-4bit --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --instruction-part '### Instruction:' --response-part '### Response:' \
    --export-merged-16bit --merged-output-dir /storage/deepseek6.7b-cs-aug2-builtins/merged_16bit
```

Equivalent wrapper: `INSTR_PART="### Instruction:" RESP_PART="### Response:" ./train_any.sh deepseek6.7b-cs-aug2-builtins /storage/hf/hub/models--deepseek-ai--deepseek-coder-6.7b-instruct/snapshots/e5d64addd26a6a1db0f9b863abf6ee3141936807 32 64 data_cs_aug2_builtins 3`

## Check status
```bash
ssh claw@claw2.tailfe3968.ts.net \
  "docker ps -a --filter name=aether-train-deepseek6.7b-cs-aug2-builtins --format '{{.Status}}'; \
   tail -30 /storage/aether_retrain_queue/deepseek6.7b-cs-aug2-builtins.log 2>/dev/null; \
   ls /storage/deepseek6.7b-cs-aug2-builtins/merged_16bit/ 2>/dev/null"
```
On-rig durable copy: `/storage/deepseek6.7b-cs-aug2-builtins/LAUNCH_COMMAND.md`.
