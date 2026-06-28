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

## NOT recovered (reconstructed from project memory — VERIFY before use)

The surviving `run_metadata.json` does NOT capture these, and they are the fragile,
Blackwell-specific parts that made this run hard. From the `gb10-moe-training-recipe`
memory, the working stack for `qwen3_5_moe` on the GB10 Blackwell was:

- **Image:** NOT `aether-unsloth-qwen3-coder-30b:q3`. The Qwen3.6 hybrid needed a
  newer base. Memory: built on `nvcr.io/nvidia/pytorch:26.05-py3`. On claw2 the
  candidate images are `aether-unsloth-qwen3-coder-30b:base26-fla` and
  `:tf520-fla` (the `-fla` = flash-linear-attention variants). **Confirm which one.**
- **`attn_implementation="sdpa"`** passed to `FastLanguageModel.from_pretrained`
  (the run tag `q36-sdpa` reflects this). The hybrid's full-attention layers call
  `flash_attn` whose `varlen_fwd` does an illegal memory access on sm_121 → must route
  through cuDNN SDPA. Fallback: `pip uninstall flash-attn`.
- **flash-linear-attention from GIT** (`git+https://github.com/fla-org/flash-linear-attention`),
  not PyPI (PyPI 0.5.1 is a stub missing `fla.ops`/`fla.modules`).
- **Version squeeze** (`--no-deps`): `transformers==5.2.0 trl==0.22.2 unsloth
  unsloth_zoo bitsandbytes==0.49.2`, `tokenizers>=0.22,<=0.23`, `BNB_CUDA_VERSION=130`.
- **Driver** `nvidia-driver-595-open`.
- Pace ~133s/step; a 150-step / 3-epoch run ≈ 5–6h.

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
