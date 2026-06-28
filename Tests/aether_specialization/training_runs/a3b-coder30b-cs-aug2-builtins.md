# Training run: a3b-coder30b-cs-aug2-builtins

Qwen3-Coder-30B-A3B (MoE, `Qwen3MoeForCausalLM`, 128 experts) Aether fine-tune on the
`out_cs_aug2_builtins` corpus. Re-run of the prior `a3b-ml1xL` recipe
(`runs/sft-a3b-ml1xL/run_metadata.json`), data dir swapped to `data_cs_aug2_builtins`.

- **Launched:** 2026-06-28 (via `retrain_queue.sh`, queue position 1)
- **Rig:** claw2 GB10 (`claw2.tailfe3968.ts.net`, user `claw`)
- **Run name / tag:** `a3b-coder30b-cs-aug2-builtins` · container `aether-train-a3b-coder30b-cs-aug2-builtins`
- **Base:** `/storage/archive/qwen3-coder-30b-a3b-model-mirror` (arch `Qwen3MoeForCausalLM`, present, 16 shards)
- **Markers:** ChatML — trainer defaults `<|im_start|>user\n` / `<|im_start|>assistant\n`
  (probe-confirmed; NO `INSTR_PART`/`RESP_PART` override). This is the STANDARD Qwen3MoE,
  NOT the fragile hybrid `qwen3_5_moe` (q36), so it uses the normal `:q3` image and hits
  none of the Blackwell linear-attention walls (memory: Qwen3-Coder-30B-A3B verified
  end-to-end on `:q3`).
- **Recipe:** r=32, alpha=64, epochs=3, lr=1e-4, bs=1, ga=8, `--no-load-in-4bit`
- **Export → `/storage/a3b-coder30b-cs-aug2-builtins/merged_16bit`**

## Exact docker run

```bash
docker run -d --name aether-train-a3b-coder30b-cs-aug2-builtins \
  --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v /home/claw/training/aether-qwen-coder-30b-unsloth:/workspace \
  -v /storage:/storage -e HF_HOME=/storage/hf \
  aether-unsloth-qwen3-coder-30b:q3 \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /storage/archive/qwen3-coder-30b-a3b-model-mirror \
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-a3b-coder30b-cs-aug2-builtins \
    --no-load-in-4bit --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir /storage/a3b-coder30b-cs-aug2-builtins/merged_16bit
```

Equivalent wrapper: `./train_any.sh a3b-coder30b-cs-aug2-builtins /storage/archive/qwen3-coder-30b-a3b-model-mirror 32 64 data_cs_aug2_builtins 3`

## Check status
```bash
ssh claw@claw2.tailfe3968.ts.net \
  "docker ps -a --filter name=aether-train-a3b-coder30b-cs-aug2-builtins --format '{{.Status}}'; \
   tail -30 /storage/aether_retrain_queue/a3b-coder30b-cs-aug2-builtins.log 2>/dev/null; \
   ls /storage/a3b-coder30b-cs-aug2-builtins/merged_16bit/ 2>/dev/null"
```
Healthy: `docker logs` shows `Filter … N/N` (non-zero) → `Num examples … Total steps … Trainable parameters`.
On-rig durable copy: `/storage/a3b-coder30b-cs-aug2-builtins/LAUNCH_COMMAND.md`.
