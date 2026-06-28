# Training run: q36-aether / q36-sdpa (RECONSTRUCTED — PARTIAL)

> **RECONSTRUCTED FROM SURVIVING METADATA — the exact launch command was LOST when the
> container was pruned.** This is the Qwen3.6-35B-A3B hybrid-MoE Aether fine-tune that
> backs the deployed `q36-aether:latest` ollama model (claw1) via `/storage/q36-sdpa`.
> The hyperparameters below are recovered from the trainer-written
> `runs/sft-q36-sdpa/run_metadata.json` (which survived). The image tag, the
> Blackwell-specific build/runtime flags, and the corpus contents are NOT in that
> metadata and are reconstructed from project memory — **verify before relying on them
> for the deferred retrain.** Status: **DEFERRED** (do not launch yet, per owner).

- **Rig:** claw2 GB10 (`claw2.tailfe3968.ts.net`, user `claw`) — Blackwell sm_121
- **Run name / tag:** `q36-sdpa` (deployed as `q36-aether:latest`)
- **Base model:** `/storage/models/Qwen3.6-35B-A3B-Instruct` (arch `qwen3_5_moe`,
  hybrid linear-attention + MoE)
- **Trained:** ~2026-06-24
- **Merged export → `/storage/q36-sdpa/merged_16bit`**

## Recovered hyperparameters (from surviving run_metadata.json — RELIABLE)

| Setting | Value |
|---|---|
| Base model (`--model-id`) | `/storage/models/Qwen3.6-35B-A3B-Instruct` |
| Data dir | `data_qwen_ml2x` (instruction + repair JSONL) — an OLDER corpus, NOT the cs-aug line |
| train_records / eval_records | 396 / 12 |
| LoRA r | `32` |
| LoRA alpha | `64` |
| LoRA dropout | `0.0` |
| target_modules | `q_proj k_proj v_proj o_proj gate_proj up_proj down_proj` |
| Epochs | `3` |
| Learning rate | `1e-4` |
| Batch size / device | `1` |
| Grad accumulation | `8` |
| Quantization | `load_in_4bit = true` (NOTE: differs from the Mistral cs-aug runs, which use `--no-load-in-4bit`) |
| max_seq_length | `1408` (auto; longest record 1276) |
| gpu_memory_utilization | `0.92` |
| device_map | `cuda:0` |
| train_on_responses_only | `true` |
| markers (INSTR/RESP) | **NOT recorded in metadata.** Qwen ChatML default is `<|im_start|>user\n` / `<|im_start|>assistant\n`; the trainer-script defaults are ChatML, so the run most likely used the defaults (no INSTR_PART/RESP_PART override). VERIFY against the model's chat template before launch. |
| include_raw_corpus / include_reference | `false` / `false` |
| Export | `export_merged_16bit = true` → `/storage/q36-sdpa/merged_16bit` |

## Reconstructed runtime — now CONFIRMED (2026-06-28)

These were not in `run_metadata.json` but have been pinned down by cross-referencing the
image inventory + the run's own README framework-versions block. **High confidence:**

- **Image: `aether-unsloth-qwen3-coder-30b:base26-fla`** (id `41d24de8b448`). CONFIRMED:
  the q36-sdpa README records `Transformers 5.2.0 / PyTorch 2.12.0a0+nv26.5 / TRL 0.22.2 /
  Datasets 4.8.5 / Tokenizers 0.22.2`, and `base26-fla` contains EXACTLY that stack
  (torch 2.12.0+nv26.05, transformers 5.2.0, **fla 0.5.2 with `fla.ops` present**, trl
  0.22.2); it was built 2026-06-24 08:43, the same day q36-sdpa trained (~10:25–13:02).
  The other candidate `:tf520-fla` (torch 2.10/nv25.11, fla 0.5.1) does NOT match and is
  the older stack the memory says hit step-0 illegal-memory. The base `:q3` image is
  torch 2.10/nv25.11 and cannot run `qwen3_5_moe`.
- **`attn_implementation="sdpa"` is HARDCODED in the trainer** — `unsloth_qwen_coder_30b_sft.py`
  passes `attn_implementation="sdpa"` to `FastLanguageModel.from_pretrained` (lines ~279 and
  ~444). So q36 used the SAME script as every other run; NO special variant or flag is
  needed for SDPA. (Why it matters: the hybrid's full-attention layers call `flash_attn`
  whose `varlen_fwd` illegal-accesses on sm_121; SDPA routes through cuDNN. Fallback if
  unsloth ever overrides it: `pip uninstall flash-attn` in the image.)
- **flash-linear-attention** is already baked into `base26-fla` as 0.5.2 with `fla.ops`
  (the git build, not the PyPI stub) — no pip step needed at launch.
- **Driver** `nvidia-driver-595-open` (already on claw2). Pace ~133s/step; 3 epochs ≈ 5–6h.

### Full reconstructed launch (READY FOR REVIEW — do NOT run until owner go-ahead)

To reproduce the ORIGINAL run (older `data_qwen_ml2x` corpus, r=32/α=64/ep3):

```bash
docker run -d --name aether-train-q36-cs-aug2-builtins \
  --gpus all --ipc=host --shm-size=16g \
  --ulimit memlock=-1 --ulimit stack=67108864 \
  -v /home/claw/training/aether-qwen-coder-30b-unsloth:/workspace \
  -v /storage:/storage -e HF_HOME=/storage/hf \
  aether-unsloth-qwen3-coder-30b:base26-fla \
  python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
    --model-id /storage/models/Qwen3.6-35B-A3B-Instruct \
    --instruction-jsonl /workspace/data_cs_aug2_builtins/aether_instruction_sft.jsonl \
    --repair-jsonl /workspace/data_cs_aug2_builtins/aether_repair_sft.jsonl \
    --output-dir /workspace/runs/sft-q36-cs-aug2-builtins \
    --no-load-in-4bit --epochs 3 --lora-r 32 --lora-alpha 64 \
    --learning-rate 1e-4 --batch-size 1 --grad-accum 8 \
    --export-merged-16bit --merged-output-dir /storage/q36-cs-aug2-builtins/merged_16bit
```

(Above swaps the data dir to the new `data_cs_aug2_builtins` — that part IS a new config,
see Caveats. Markers: Qwen ChatML defaults, no override; probe the base's chat template to
confirm before launch.) NOTE: `train_any.sh` would NOT use `base26-fla` unless you pass
`IMG=aether-unsloth-qwen3-coder-30b:base26-fla` — so prefer the explicit `docker run` above,
or `IMG=… ./train_any.sh q36-cs-aug2-builtins /storage/models/Qwen3.6-35B-A3B-Instruct 32 64 data_cs_aug2_builtins 3`.

### Residual risk (why this is report-not-auto-launch)
Even with the image pinned, the GB10 Blackwell walls (gb10-moe-training-recipe.md) can
resurface on a fresh launch. If it step-0 illegal-accesses, `CUDA_LAUNCH_BLOCKING=1` pins
the kernel; the `bincount`/Triton errors are red herrings. Watch the first step actually
advance before trusting the run. **The very first successful q36 launch MUST write its exact
`docker run` here + to `/storage/q36-*/LAUNCH_COMMAND.md`** so it is never lost again.

## Caveats for the deferred retrain

- **The Q8 GGUF of this model is BROKEN** (hybrid `qwen3_5_moe` linear-attention tensors
  corrupt under Q8 → degenerate loops); it must be served as bf16. Do not export Q8.
- This run used `data_qwen_ml2x` (r=32/α=64), NOT the cs-aug corpus line. A retrain on a
  newer corpus (e.g. `out_cs_aug2_builtins`) would be a NEW configuration, not a
  reproduction — decide corpus + r/α deliberately.
- Because the exact command was lost, the FIRST step of any q36 retrain should be to
  pin the image + the four Blackwell flags above by actually building/launching once and
  then writing the resulting exact `docker run` here (and to `/storage/q36-*/`), the same
  way `mistral24b-cs-aug2-builtins.md` was captured.
