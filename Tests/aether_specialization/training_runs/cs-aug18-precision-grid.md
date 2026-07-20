# Training run: cs-aug18-precision-grid

6-run 2x3 grid extending the bf16-vs-4bit "Precision experiment" section of
`aether_specialization_findings.md` with the missing 8-bit data point, re-run on the
current (larger) corpus instead of the old `cs-aug4` one. 2 models
(`qwen3-8b-nothink`, `qwen35-9b`) x 3 precisions (4bit / 8bit / 16bit-bf16), same
recipe as the original precision experiment.

- **Launched:** 2026-07-19 (via `retrain_queue_cs_aug18_precision.sh`, sequential on
  claw2's single GPU)
- **Rig:** claw2 GB10 · containers `aether-train-<tag>` (see tags below)
- **Corpus:** `data_cs_aug18` (`Tests/aether_specialization/out_cs_aug18`) — reused
  as-is rather than regenerated, since it was already built fresh against the current
  `corpus_candidates_manifest.json` (`aether_version 2026-07-19-9`, 649 instruction /
  38 repair records) moments before this queue was written — see
  `aether-corpus-verification` memory for the regression check that produced it.
- **Recipe (all 6 runs):** r=32, alpha=64, epochs=3, lr=1e-4, bs=1, ga=8, ChatML
  default instruction/response markers (no override needed for either model family).
  Identical to the `qwen3-8b-nothink`/`qwen35-9b` recipe used in the original
  `cs-aug4` precision experiment.
- **8-bit flag:** `--load-in-8bit`, newly added to `unsloth_qwen_coder_30b_sft.py`
  (mirrors `--load-in-4bit` plumbing through both `FastLanguageModel.from_pretrained`
  call sites; mutually exclusive with `--load-in-4bit`, enforced at arg-parse time).
  Verified empirically before use: Unsloth `2026.6.6` in this image genuinely supports
  `load_in_8bit=True` for both `Qwen3ForCausalLM` (via the `:q3` image, transformers
  4.56.2) and `qwen3_5` hybrid-attention arch (via the `:tf520-fla` image, transformers
  5.2.0 — required for `qwen3_5`, which `:q3`'s older transformers doesn't recognize at
  all) — confirmed via a real LoRA forward+backward step (loss/grad_norm computed) for
  both, not just a load-and-exit check.
- **Images:** `qwen3-8b-nothink-*` uses `aether-unsloth-qwen3-coder-30b:q3`;
  `qwen35-9b-*` uses `aether-unsloth-qwen3-coder-30b:tf520-fla` (required for the
  `qwen3_5` architecture — matches whatever image the original `qwen35-9b-cs-aug4`
  runs must have used, since `:q3`'s transformers can't load a `qwen3_5` config at
  all).
- **Base models:**
  - `qwen3-8b-nothink`: `/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218`
  - `qwen35-9b`: `/storage/models/Qwen3.5-9B`
- **Export →** `/storage/<tag>/merged_16bit` for each of the 6 tags:
  `qwen3-8b-nothink-4bit-cs-aug18`, `qwen3-8b-nothink-8bit-cs-aug18`,
  `qwen3-8b-nothink-16bit-cs-aug18`, `qwen35-9b-4bit-cs-aug18`,
  `qwen35-9b-8bit-cs-aug18`, `qwen35-9b-16bit-cs-aug18`.

## Exact docker run (per tag; queue script fills in `$TAG`/`$IMG`/`$MODEL`/precision flags)

```bash
docker run -d --name aether-train-$TAG --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v /home/claw/training/aether-qwen-coder-30b-unsloth:/workspace \
  -v /storage:/storage -e HF_HOME=/storage/hf \
  $IMG \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id $MODEL \
    --instruction-jsonl /workspace/data_cs_aug18/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug18/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-$TAG \
    [--load-in-8bit --no-load-in-4bit  |  --no-load-in-4bit  |  <nothing, 4bit is default>] \
    --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir /storage/$TAG/merged_16bit
```

Full script: `retrain_queue_cs_aug18_precision.sh` (claw2,
`~/training/aether-qwen-coder-30b-unsloth/`) — resumable (skips tags whose merged
export already exists), writes `/storage/<tag>/LAUNCH_COMMAND.md` per run, logs to
`/storage/aether_retrain_queue_cs_aug18_precision/queue.log`.

## Check status
```bash
ssh claw@claw2 \
  "tail -40 /storage/aether_retrain_queue_cs_aug18_precision/queue.log; \
   docker ps -a --filter name=aether-train- --format '{{.Names}}\t{{.Status}}'; \
   ls /storage/*cs-aug18*/merged_16bit/*.safetensors 2>/dev/null | sed 's#/merged_16bit.*##' "
```
On-rig durable copy of each launch command: `/storage/<tag>/LAUNCH_COMMAND.md`.
Running status note (queued/running/done/benchmarked): `training_runs/cs-aug18-precision-grid-status.md`.
