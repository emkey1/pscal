# Aether 30B Specialization — Corrected Recipe (v7)

This is the corrected training recipe after the v6 ("numbered + small-only")
run drove no-guide accuracy to **0/25**. It supersedes the "numbered raw corpus
cases only" policy described in `aether_training_operations.md`.

Read that companion doc for the host/workspace/serving topology. This doc is
only about **what to train on, how, and how to measure it honestly.**

## TL;DR

- **Goal (the KPI):** the model writes correct Aether with **no guide in the
  prompt** (the `none` column of the doc-bench).
- **What broke v6:** it trained an *Instruct* model on bare Aether source
  completions with **zero instruction-formatted supervision**, selected the
  checkpoint by raw-corpus eval loss, masked nothing, and ran 8 epochs. It
  learned to *echo* Aether when primed, not *produce* it on request.
- **The fix:** train on compiler-verified **instruction pairs** (each corpus
  case → request→Aether), mask the prompt, hold the eval split out of the
  *instruction* set, fewer/gentler epochs, export a **merged 16-bit** checkpoint
  for vLLM + NVFP4.
- **Plus:** the benchmark was **contaminated** with the training data
  (23/25 tasks overlap). v7 de-contaminates by default so `tasks.json` is an
  honest held-out test.

## Why v6 produced "essentially nothing"

Measured exact-stdout match (out of 25), by in-context guide:

| Run | full | small | **none (KPI)** |
|---|---:|---:|---:|
| v2 | 19 | 18 | **15–17** |
| v5 | 20 | 20 | **8** |
| **v6** | 21 | 16 | **0** |

The `none` column collapses as training leans harder into raw-corpus echoing.
Root causes, all in code:

1. **No instruction signal.** `aether_specialization_prepare_assets.py` hard-wrote
   the instruction/repair JSONL to empty strings. The trainer's entire dataset
   was bare corpus source + guide chunks as plain-text completions.
2. **Wrong eval/selection.** `unsloth_qwen_coder_30b_sft.py` held the eval split
   out of the **raw corpus** and used `load_best_model_at_end` on that eval loss,
   i.e. it selected the *most* over-specialized checkpoint.
3. **No loss masking.** Loss covered the whole sequence, not the assistant span.
4. **Over-fit schedule.** 8 epochs at `lr=2e-4` over ~130 tiny records.

## Canonical state (so it stops drifting)

| | Reality |
|---|---|
| KPI | `none` column of `Tests/aether_doc_bench/tasks.json` |
| Best prior model on KPI | **v2** (but its number was inflated by benchmark contamination) |
| v6 served at `:8017` | worst on KPI (0/25) — do not treat as canonical |
| Correct dataset generator | `Tools/aether_specialization_build_dataset.py` (verified instruction pairs) |
| v7 training data | instruction-only, de-contaminated: **99 instruction + 10 repair = 109** verified records |
| Ultimate artifact | NVFP4 export via `tools/modelopt_nvfp4_export.py` |

## The v7 recipe

### Data (instruction-only, compiler-verified, de-contaminated)

`prepare_assets.py` now runs `build_dataset.py` and **refuses to emit empty
instruction JSONL**. Each canonical `corpus_candidates` case is promoted to a
chat record: system + user (request synthesized from manifest tags/notes/stdout)
+ assistant (the verified source). Seed instruction and repair pairs are added.
Every program is compiled and run before it is kept.

- With de-contamination (default): **99 instruction + 10 repair**.
- Records whose exact `expected_stdout` matches a `tasks.json` task are dropped
  (30 of them) so the benchmark stays honest. Toggle with
  `--include-benchmark-overlap` (then 123 + 16, but a dirty metric).
- Raw corpus + small guide are still exported for provenance but are **not**
  language-modeled as bare completions (that was a primary v6 failure).

### Trainer changes (`tools/unsloth_qwen_coder_30b_sft.py`)

- Eval split is held out of the **supervised/instruction** records, so
  `eval_loss` and best-checkpoint selection track instruction-following.
- `--train-on-responses-only` (default **on**): loss is computed on the assistant
  Aether only, using Qwen ChatML markers `<|im_start|>user\n` /
  `<|im_start|>assistant\n`.
- `--include-raw-corpus` / `--include-reference` default **off**.
- Defaults: **`epochs=3`, `lr=1e-4`** (down from 8 / 2e-4). LoRA `r=16, α=32`.
- **`--export-merged-16bit` (default on)** writes a merged HF checkpoint
  (`save_pretrained_merged(..., save_method="merged_16bit")`) — the vLLM- and
  modelopt-loadable artifact. Point `--merged-output-dir` at `/storage`
  (~57 GiB; `/` is the box that ran out of disk in v6). GGUF default off.
- `run_metadata.json` now records the policy (masking, includes, eval source,
  merged dir) so every run is self-describing.

### Remote runner changes (`tools/spark_unsloth_qwen_coder_train_remote.py`)

- Defaults `epochs=3`; passes `--learning-rate` (default `1e-4`).
- Mounts `/storage` into the container and passes
  `--merged-output-dir /storage/aether-qwen-coder-30b-<run>/merged_16bit`.

## Runbook (end to end)

> Prereq: free the GPU — stop `ray-worker` and `ComfyUI` on `claw2` first
> (keep them in boot policy, just not running). Confirm
> `/home/claw/training/aether-qwen-coder-30b-unsloth/model-mirror` exists or pass
> `--allow-network-download` to the trainer.

```bash
# 1. Build + verify assets locally (de-contaminated by default).
python3 Tools/aether_specialization_prepare_assets.py \
  --output-dir Tests/aether_specialization/out
#   -> expect: instruction_records=99 repair_records=10
#   -> the run prints excluded_benchmark_overlap instruction=24 repair=6

# 2. Sync + train on claw2 (regenerates+rsyncs assets, syncs trainer, builds image, starts).
python3 tools/spark_unsloth_qwen_coder_train_remote.py \
  --run-name sft-qwen-coder-30b-v7-instruction \
  start
python3 tools/spark_unsloth_qwen_coder_train_remote.py \
  --run-name sft-qwen-coder-30b-v7-instruction status   # tail logs

# 3. (Trainer already exported) merged 16-bit lands at:
#    /storage/aether-qwen-coder-30b-sft-qwen-coder-30b-v7-instruction/merged_16bit

# 4. Serve that merged dir with vLLM and benchmark full/small/none with
#    Tools/aether_doc_bench.py. Gate on the `none` column (see below).

# 5. NVFP4 export (ultimate artifact). --source-model MUST be the v7 merged dir.
python3 tools/spark_modelopt_nvfp4_remote.py \
  --source-model /storage/aether-qwen-coder-30b-sft-qwen-coder-30b-v7-instruction/merged_16bit \
  --run-name qwen-coder-30b-v7-nvfp4 prepare
python3 tools/spark_modelopt_nvfp4_remote.py --run-name qwen-coder-30b-v7-nvfp4 setup-venv
python3 tools/spark_modelopt_nvfp4_remote.py \
  --source-model /storage/aether-qwen-coder-30b-sft-qwen-coder-30b-v7-instruction/merged_16bit \
  --run-name qwen-coder-30b-v7-nvfp4 start
python3 tools/spark_modelopt_nvfp4_remote.py --run-name qwen-coder-30b-v7-nvfp4 status
```

`prepare` syncs `Tests/aether_specialization/out/aether_instruction_sft.jsonl`
(+repair) as NVFP4 calibration data, so run step 1 first. Default qformat is
`nvfp4_experts_only` (FP4 on the MoE experts only).

## Honest measurement

Prior v2/v5/v6 numbers were measured on a benchmark their training overlapped,
so they are **not** comparable to v7's clean numbers. To establish an honest
baseline:

1. Benchmark the **base** `Qwen3-Coder-30B-A3B-Instruct` (no adapter) on
   `tasks.json` for full/small/none — the honest floor.
2. Benchmark **v7** for full/small/none.
3. **Gate:** v7 `none` > base `none`, and v7 `none` ≫ 0. v7 full/small ≥ base.
4. Do **not** compare v7 `none` to v2's 15–17 (those leaked the eval).

**Recommended follow-up:** author a small fresh held-out task set (10–20 verified
Aether tasks *not* derived from `corpus_candidates`) for a clean generalization
read independent of `tasks.json`.

## Caveats / known issues (not fixed here)

- **`tools/` vs `Tools/` case collision.** Git tracks Aether scripts under both
  prefixes; on macOS (case-insensitive) they're one directory, so it works, and
  the remote runner reads the trainer locally then writes a fixed remote path —
  training is unaffected. But a fresh clone on Linux gets two split directories.
  Fix by consolidating to one case before anyone clones on Linux.
- **Prompt-masking smoke test.** `train_on_responses_only` and the merged export
  cannot be run on the Mac (no GPU/unsloth). Before the full run, confirm in the
  container that the import resolves and the ChatML markers match the tokenizer,
  or pass `--no-train-on-responses-only` to fall back.
- **Scratch files to remove** (untracked, left by prior tooling — not deleted
  automatically): `Tests/aether_doc_bench/does_not_exist.local.json`,
  `Tests/aether_doc_bench/destinations.spark_qwen_coder_30b_v2_command.json`,
  `Tests/aether_doc_bench/destinations.spark_qwen_coder_30b_v2_vllm.json`,
  and `.cache/`.
- **vLLM readiness.** Serving the merged checkpoint from `/storage` (NFS) is slow
  to become ready (load + torch.compile + warmup). "Container up" ≠ "endpoint up."

## Iteration results and ceiling analysis (v7 → v7.3)

After the recipe fix, a corpus-augmentation loop was run (de-contaminated,
5 epochs/run from v7.1 on). `none` (the KPI) trajectory:

| run | corpus instr/repair | none | change |
|---|---|---:|---|
| v6 | 0 (empty) | 0/25 | the broken baseline |
| v7 | 99 / 10 | 8/25 | recipe fix, 3 epochs |
| v7.1 | 104 / 10 | **14/25** | +5 large/TOON/module programs, 5 epochs |
| v7.2 | 115 / 10 | 12/25 | +11 more verified programs |
| v7.3 | 118 / 13 | 12/25 | +3 rich validators, +3 targeted repair pairs |

Guide-in-context stayed strong throughout but did **not** improve with more
training — full 20–23/25, small 21–22/25 across versions, with v7 (the first,
least-trained corrected model) peaking at **full 23 / small 22** and v7.3
slipping to 20/21. The whole gain on `none` came from v7→v7.1 (recipe + first
corpus); further verified, targeted data plateaued at ~12–14 on `none` (±2 is
temp-0.2 sampling noise — the tasks that flip between runs are simple TOON ones)
and mildly eroded guide-in-context (overfitting on the tiny set). **v7.1 is the
sweet spot: best `none` (14) and least over-trained of the 5-epoch runs.**

**Why it plateaus — the remaining failures are base-prior errors, not missing
examples.** Reading the model's actual generations on the persistent failures:
- `module_*`: references the module name as an identifier (`mod.fn()`) instead of
  `use "mod"; fn()` (SCOPE-001)
- `toon_jobs_summary`: `toon_key(doc, …)` instead of `toon_root(doc)` first (TOON-001)
- `contract_normalize`: `@pre` placed inside the function body (ANN-001)
- `large_*`: now compile and run, but miss output detail (variadic-int spacing
  `"( 0)"` vs `"(0)"`, off-by-one severity logic)

These are the base model's pretraining instincts overriding Aether's rules.
Three rounds of correct examples and explicit error→fix repair pairs did not
reliably dislodge them — so this is a model-prior ceiling, not a data-quantity
problem.

**Levers not yet pulled (for `none` > 14 in future work):**
- A larger, sustained de-contaminated corpus (hundreds of examples accrued over
  time, not the ~35 added here).
- Preference/RL on the task distribution — directly optimizes exact-match, which
  SFT does not.
- Higher LoRA rank / more epochs — but note `eval_loss` anti-correlates with
  `none` here (v7.3 had the lowest eval_loss yet not the best `none`), so
  selecting the checkpoint by instruction `eval_loss` is a weak proxy for the KPI;
  a held-out *none-style* eval for checkpoint selection would help.
- A stronger/larger base model.
- A fresh held-out benchmark (n=25 at temp 0.2 is noisy; ±2 is not signal).

**Best checkpoint:** v7.1 had the peak measured `none` (14); v7.3 has the richest
training (incl. repair coverage) and is the more robust bet beyond this 25-task
eval. The two are within benchmark noise — either is a fine source for the NVFP4
export. Merged checkpoints live at
`/storage/aether-qwen-coder-30b-sft-qwen-coder-30b-v7{,1,2,3}/merged_16bit`.
