# Training run: mistral24b-cs-aug2-builtins

Durable, reproducible recipe for the Mistral-Small-24B Aether specialization fine-tune
trained on the `out_cs_aug2_builtins` corpus. Captured at launch from `docker inspect`
of the live container so it can never be lost to container pruning.

- **Launched:** 2026-06-28
- **Rig:** claw2 GB10 (`claw2.tailfe3968.ts.net`, user `claw`)
- **Run name / tag:** `mistral24b-cs-aug2-builtins`
- **Container name:** `aether-train-mistral24b-cs-aug2-builtins`
- **Base model:** `mistralai/Mistral-Small-24B-Instruct-2501`
  (local snapshot, see `--model-id` below)
- **Corpus:** `Tests/aether_specialization/out_cs_aug2_builtins/`
  (dataset version `2026-06-28-1`, aether_version `2026-06-27-3`; 406 instruction +
  25 repair records; supersedes `out_cs_aug2`, adds the `aether_builtins_reference`
  and migrates to the FX-001 effect model). 419 train / 12 eval after holdout.
- **Merged export → `/storage/mistral24b-cs-aug2-builtins/merged_16bit`**

This is a faithful re-run of the prior `mistral24b-cs-aug2` (which trained on
`out_cs_aug2`) with the IDENTICAL recipe, pointed at the newer builtins corpus.
The exact prior recipe was recovered from `runs/sft-mistral24b-cs-aug2/run_metadata.json`
and `docker inspect aether-train-mistral24b-cs-aug2`.

## Reproduce: the wrapper invocation (preferred)

On claw2, from `~/training/aether-qwen-coder-30b-unsloth`, after the corpus is synced
to `data_cs_aug2_builtins/` (see "Corpus sync" below):

```bash
INSTR_PART="[INST]" RESP_PART="[/INST]" \
  ./train_any.sh mistral24b-cs-aug2-builtins \
    /storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724/ \
    64 128 data_cs_aug2_builtins 4
```

`train_any.sh <tag> <model> <r> <alpha> <data> <ep>`. NOTE: `train_any.sh` defaults
are `r=32 alpha=64` — this run EXPLICITLY uses `r=64 alpha=128`, so they must be passed.
`INSTR_PART`/`RESP_PART` are REQUIRED for Mistral (non-Qwen); omitting them masks every
label to -100 → "Removed all N samples → num_samples=0" crash at ~4min.

## Reproduce: the exact `docker run` (canonical, from `docker inspect`)

```bash
docker run -d --name aether-train-mistral24b-cs-aug2-builtins \
  --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v /home/claw/training/aether-qwen-coder-30b-unsloth:/workspace \
  -v /storage:/storage \
  -e HF_HOME=/storage/hf \
  aether-unsloth-qwen3-coder-30b:q3 \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724/ \
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-mistral24b-cs-aug2-builtins \
    --no-load-in-4bit \
    --epochs 4 \
    --lora-r 64 \
    --lora-alpha 128 \
    --learning-rate 1e-4 \
    --batch-size 1 \
    --grad-accum 8 \
    --instruction-part "[INST]" \
    --response-part "[/INST]" \
    --export-merged-16bit \
    --merged-output-dir /storage/mistral24b-cs-aug2-builtins/merged_16bit
```

## Exact configuration

| Setting | Value |
|---|---|
| Image | `aether-unsloth-qwen3-coder-30b:q3` |
| Trainer script | `/workspace/scripts/unsloth_qwen_coder_30b_sft.py` (Unsloth QLoRA) |
| Base model (`--model-id`) | `/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724/` |
| Data dir | `data_cs_aug2_builtins` (instruction + repair JSONL) |
| LoRA r | `64` |
| LoRA alpha | `128` |
| LoRA dropout | `0.0` |
| target_modules | `q_proj k_proj v_proj o_proj gate_proj up_proj down_proj` |
| Epochs | `4` |
| Learning rate | `1e-4` |
| Batch size / device | `1` |
| Grad accumulation | `8` (effective batch 8) |
| Quantization | `--no-load-in-4bit` (bf16 LoRA; merges clean for serving) |
| max_seq_length | auto (longest record ~1210 → cap 1280) |
| train_on_responses_only | `true`, markers `[INST]` / `[/INST]` |
| include_raw_corpus / include_reference | `false` / `false` |
| Export | `--export-merged-16bit` → `/storage/mistral24b-cs-aug2-builtins/merged_16bit` |
| Mounts | `…/aether-qwen-coder-30b-unsloth:/workspace`, `/storage:/storage` |
| HostConfig | `ipc=host`, `shm-size=16g`, `ulimit memlock=-1`, `ulimit stack=67108864` |
| Env | `HF_HOME=/storage/hf` |

## Corpus sync (run before launch)

The `out_cs_aug2_builtins` corpus is pre-built locally. Copy the 4 `aether_*` data
files (+ `aether_training_mix.json` for provenance) into a new data dir on claw2.
Use an ABSOLUTE remote path — `rsync host:relpath` mangles the target.

```bash
SRC=/Users/mke/PBuild/Tests/aether_specialization/out_cs_aug2_builtins/
HOST=claw@claw2.tailfe3968.ts.net
RHOME=$(ssh "$HOST" 'printf %s "$HOME"')
rsync -av "$SRC" "${HOST}:${RHOME}/training/aether-qwen-coder-30b-unsloth/data_cs_aug2_builtins/"
```

## Check status

```bash
ssh claw@claw2.tailfe3968.ts.net \
  "docker ps -a --filter name=aether-train-mistral24b-cs-aug2-builtins --format '{{.Status}}'; \
   docker logs --tail 40 aether-train-mistral24b-cs-aug2-builtins 2>&1 | tail; \
   ls -la /storage/mistral24b-cs-aug2-builtins/merged_16bit/ 2>/dev/null"
```

Done when `merged_16bit/` holds the `model-*.safetensors` shards and the container is
`Exited (0)`. Healthy-launch signature in `docker logs`: 10/10 checkpoint shards →
`Unsloth: Tokenizing` → `Filter … 419/419` (NOT "Removed all") → `Num examples = 419 |
Num Epochs = 4 | Total steps = 212 | Trainable parameters = 369,623,040 (1.54%)`.

## On-rig durable copy

A copy of this recipe (and the auto-written `run_metadata.json`) lives on persistent
storage at `/storage/mistral24b-cs-aug2-builtins/` so it survives container pruning:
`/storage/mistral24b-cs-aug2-builtins/LAUNCH_COMMAND.md` (exact command) alongside the
trainer-written `/storage/mistral24b-cs-aug2-builtins/merged_16bit/` and
`runs/sft-mistral24b-cs-aug2-builtins/run_metadata.json`.
