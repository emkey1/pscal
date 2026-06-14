# Aether Training And Benchmark Operations

This document records the current and recent Aether specialization and
benchmark infrastructure as it actually exists in this repository and on the
remote systems used for training and inference.

It is intentionally operational and specific.

## Local repository

- Repository root: `/Users/mke/PBuild`
- Current branch at time of writing: `AetherLang`
- Primary local machine role:
  - edit Aether/compiler/docs/corpus
  - prepare specialization assets
  - launch remote training
  - launch remote serving
  - run local benchmark harness against remote endpoints

## Important local directories

- Aether training corpus:
  - `/Users/mke/PBuild/Tests/aether_specialization/corpus_candidates`
- Corpus manifest:
  - `/Users/mke/PBuild/Tests/aether_specialization/corpus_candidates_manifest.json`
- Corpus fixtures:
  - `/Users/mke/PBuild/Tests/aether_specialization/fixtures`
- Prepared training assets:
  - `/Users/mke/PBuild/Tests/aether_specialization/out`
- Benchmark tasks:
  - `/Users/mke/PBuild/Tests/aether_doc_bench/tasks.json`
- Benchmark reports:
  - `/Users/mke/PBuild/Tests/aether_doc_bench/out`
- Small Aether guide:
  - `/Users/mke/PBuild/Docs/aether_for_llms_with_small_contexts.md`
- Full Aether guide:
  - `/Users/mke/PBuild/Docs/aether_for_llms_and_others.md`

## Important local scripts

Corpus and asset prep:

- `/Users/mke/PBuild/Tools/aether_specialization_validate_corpus.py`
- `/Users/mke/PBuild/Tools/aether_specialization_export_corpus.py`
- `/Users/mke/PBuild/Tools/aether_specialization_export_reference_corpus.py`
- `/Users/mke/PBuild/Tools/aether_specialization_prepare_assets.py`
- `/Users/mke/PBuild/Tools/aether_specialization_sync_to_spark.py`

Training:

- `/Users/mke/PBuild/tools/unsloth_qwen_coder_30b_sft.py`
- `/Users/mke/PBuild/tools/spark_unsloth_qwen_coder_train_remote.py`
- `/Users/mke/PBuild/Tools/qwen3_base_lora_sft.py`
- `/Users/mke/PBuild/Tools/spark_qwen3_base_train_remote.py`

Benchmarking:

- `/Users/mke/PBuild/Tools/aether_doc_bench.py`
- `/Users/mke/PBuild/Tools/run_python_only_benchmark.py`
- `/Users/mke/PBuild/Tools/aether_doc_bench_combine.py`
- `/Users/mke/PBuild/Tools/run_aether_doc_bench_with_doc.py`

Other model packaging / export:

- `/Users/mke/PBuild/Tools/modelopt_nvfp4_export.py`
- `/Users/mke/PBuild/Tools/spark_modelopt_nvfp4_remote.py`

## Current training policy snapshot

At the time of this document, the most recent explicitly requested policy was:

- training corpus should contain only:
  - numbered Aether corpus cases
  - the small guide
- it should not contain:
  - full guide
  - synthetic instruction pairs
  - repair pairs
  - scratch/probe files
  - unnumbered helper/module-only corpus entries

The current local asset generation was modified to reflect that request:

- raw corpus export filters to numbered manifest entries with non-empty golden
  stdout
- reference corpus export defaults to the small guide only
- instruction and repair JSONL outputs are intentionally empty in the current
  “numbered + small only” path

## Current local prepared asset set

Prepared by:

```bash
python3 Tools/aether_specialization_prepare_assets.py \
  --output-dir Tests/aether_specialization/out
```

Current expected shape:

- `aether_raw_corpus.json`
  - numbered runnable cases only
- `aether_reference_corpus.json`
  - one item: `Docs/aether_for_llms_with_small_contexts.md`
- `aether_instruction_sft.jsonl`
  - empty for the current restricted run policy
- `aether_repair_sft.jsonl`
  - empty for the current restricted run policy
- `aether_training_mix.json`
  - summary of the intended mix

## Remote systems

### `claw2`

- SSH target that works from this environment:
  - `claw@100.124.15.16`
- Hostname on remote:
  - `claw2`
- Remote user:
  - `claw`
- Home:
  - `/home/claw`
- Filesystems observed:
  - `/` on local NVMe: about `916G`
  - `/storage` on NFS mount `c1m:/storage`: about `7.3T`

Observed at documentation time:

- `/` usage: `96%`
- `/storage` usage: `25%`

Training workspaces under:

- `/home/claw/training/aether-qwen-coder-30b-unsloth`
- `/home/claw/training/aether-qwen3-base`

Other top-level training/model dirs seen:

- `/home/claw/training/ministral14b`
- `/home/claw/training/models`

### `claw1`

- This environment currently cannot resolve `claw1` by hostname over SSH.
- Attempt from this environment failed with host connection error.
- Operationally, `claw1` has still been referenced by the user as another
  inference/training box and was used historically for larger hosted model
  tests.
- If needed, document its IP separately once verified from a working shell.

### `c1t`

- Historically used as an OpenAI-compatible inference endpoint.
- User-specified endpoint used in benchmarking:
  - `http://c1t:8001/v1`
- One recorded model there:
  - `openai/gpt-oss-120b`

### Local LM Studio

- Historically used for local OpenAI-compatible benchmarking on the laptop.
- User-provided endpoint:
  - `http://localhost:1215/v1`
- Used for multiple open-weight models.
- Not committed to repo config by design.

## Remote training workspace layout on `claw2`

Primary 30B workspace:

- `/home/claw/training/aether-qwen-coder-30b-unsloth`

Important subdirectories:

- `data`
  - receives synced training assets from local repo
- `logs`
  - one log per run
- `runs`
  - one directory per training run
- `scripts`
  - synced remote trainer scripts
- `unsloth-notebooks`
  - pinned checkout used only to obtain `Dockerfile_DGX_Spark`
- `model-mirror`
  - local mirror of the base model weights used to avoid network fetch during
    runs

## Remote container / image naming

### Image

Current Unsloth image name:

- `aether-unsloth-qwen3-coder-30b:568a161`

This comes from the pinned Unsloth notebooks commit:

- repo: `https://github.com/unslothai/notebooks.git`
- commit: `568a161218dae1c30b6e13285192dc268850dc8b`

### Training containers

Convention:

- `aether-unsloth-<run-name>`

Observed examples:

- `aether-unsloth-sft-qwen-coder-30b-v2`
- `aether-unsloth-sft-qwen-coder-30b-v5`
- `aether-unsloth-sft-qwen-coder-30b-v6-numbered-smallonly`

### Serving containers

Observed examples:

- `aether-bench-vllm`
- `aether-bench-vllm-v5`
- `aether-bench-vllm-v6`

### Other packaging containers

- `aether-modelopt-qwen-coder-30b-nvfp4-v1`
- `aether-export-v6`

## Current remote container state at documentation time

Observed on `claw2`:

- `ray-worker`
  - running
- `aether-bench-vllm-v6`
  - running
- `aether-unsloth-sft-qwen-coder-30b-v6-numbered-smallonly`
  - exited
- `aether-bench-vllm`
  - exited
- `aether-unsloth-sft-qwen-coder-30b-v2`
  - exited
- `aether-modelopt-qwen-coder-30b-nvfp4-v1`
  - exited

Operational note:

- the user explicitly wants `ray-worker` and `ComfyUI` stopped when not
  needed, but not removed from boot/startup policy

## Models and roles

### 4B path

Primary small-model specialization target:

- `Qwen/Qwen3-4B-Base`

Repo-side scaffold for that path:

- `/Users/mke/PBuild/Tools/qwen3_base_lora_sft.py`
- `/Users/mke/PBuild/Tools/spark_qwen3_base_train_remote.py`
- `/Users/mke/PBuild/Tools/spark_qwen3_base_remote.py`

Historical intent:

- establish a small, trainable Aether-native model
- compare Aether and Python task behavior
- compare with full/small/no guide

### 30B path

Current larger specialization path:

- base model id:
  - `unsloth/Qwen3-Coder-30B-A3B-Instruct`

Current trainer:

- `/Users/mke/PBuild/tools/unsloth_qwen_coder_30b_sft.py`

Current remote runner:

- `/Users/mke/PBuild/tools/spark_unsloth_qwen_coder_train_remote.py`

Method:

- Unsloth QLoRA
- `load_in_4bit=True`
- LoRA target modules:
  - `q_proj`
  - `k_proj`
  - `v_proj`
  - `o_proj`
  - `gate_proj`
  - `up_proj`
  - `down_proj`
- LoRA defaults:
  - `r=16`
  - `alpha=32`
  - `dropout=0.0`
- gradient checkpointing:
  - `"unsloth"`
- compute:
  - bf16

## Training runs: recent and current

### `sft-qwen-coder-30b-v5`

Purpose:

- first run after fixing the dataset promotion bug so compiler-verified corpus
  examples were actually entering supervised training

Important discovered behavior:

- training completed
- merged benchmarkable weights were not left directly in `final/`
- the usable merged safetensor export ended up under:
  - `/home/claw/training/aether-qwen-coder-30b-unsloth/runs/sft-qwen-coder-30b-v5/final/gguf/q8_0`

### `sft-qwen-coder-30b-v6-numbered-smallonly`

Purpose:

- test the user-requested restricted corpus:
  - numbered cases only
  - small guide only
  - no instruction/repair data

Verified training metadata:

- `train_records`: `96`
- `eval_records`: `12`
- `model_id`: `/workspace/model-mirror`
- `instruction_jsonl`: still passed but empty
- `repair_jsonl`: still passed but empty
- `corpus_json`: `/workspace/data/aether_raw_corpus.json`
- `reference_json`: `/workspace/data/aether_reference_corpus.json`
- auto-sized `max_seq_length`: `1280`

Training outcome:

- actual LoRA training completed successfully
- export to local workspace failed at first because `/` ran out of space during
  merge / GGUF generation
- adapter artifacts were still saved under:
  - `/home/claw/training/aether-qwen-coder-30b-unsloth/runs/sft-qwen-coder-30b-v6-numbered-smallonly/final`

### Export-only recovery for `v6`

Because training itself succeeded but local export failed, an export-only
recovery path was added to:

- `/Users/mke/PBuild/tools/unsloth_qwen_coder_30b_sft.py`

That export-only pass reloaded the saved adapter and wrote merged outputs to
`/storage`, not `/`.

Recovered outputs:

- merged safetensor vLLM-loadable model:
  - `/storage/aether-qwen-coder-30b-v6-numbered-smallonly/gguf/q8_0`
- GGUF file for llama.cpp:
  - `/storage/aether-qwen-coder-30b-v6-numbered-smallonly/gguf/q8_0_gguf/model-mirror.Q8_0.gguf`

## Serving / inference layout

### vLLM serving on `claw2`

Observed active port:

- `8017`

Current active container:

- `aether-bench-vllm-v6`

Current served model name:

- `aether-qwen-coder-30b-v6`

Current model path mounted into container:

- `/storage/aether-qwen-coder-30b-v6-numbered-smallonly/gguf/q8_0`

Important vLLM behavior observed:

- checkpoint load from `/storage` is slow because `/storage` is NFS
- vLLM logs report:
  - checkpoint size about `56.87 GiB`
  - NFS detected
  - no auto-prefetch because checkpoint size is too close to available RAM
- after weights load, vLLM still performs:
  - torch.compile
  - warmup
  - CUDA graph capture
  - KV cache sizing

This means “container started” is not equal to “endpoint ready.”

### OpenAI-compatible benchmark endpoint examples

Historically used benchmark endpoints:

- `http://100.124.15.16:8017/v1`
  - current v6 vLLM run on `claw2`
- `http://100.124.15.16:8016/v1`
  - earlier v5 vLLM run on `claw2`
- `http://c1t:8001/v1`
  - user-provided `gpt-oss-120b` endpoint
- `http://localhost:1215/v1`
  - local LM Studio endpoint

## Benchmark infrastructure

Primary benchmark harness:

- `/Users/mke/PBuild/Tools/aether_doc_bench.py`

Primary benchmark task manifest:

- `/Users/mke/PBuild/Tests/aether_doc_bench/tasks.json`

Primary guide variants:

- full:
  - `/Users/mke/PBuild/Docs/aether_for_llms_and_others.md`
- small:
  - `/Users/mke/PBuild/Docs/aether_for_llms_with_small_contexts.md`
- none:
  - no guide text sent

Benchmark success criteria:

- generated source?
- compiled?
- ran?
- exact stdout match?

Optional repair loop support exists in the harness, but the recent 30B runs
described here were benchmarked in first-pass mode.

## Recent benchmark report files

Important local reports mentioned by current work:

- `/Users/mke/PBuild/Tests/aether_doc_bench/out/spark_qwen_coder_30b_v5_vllm_completecorpus.json`
- `/Users/mke/PBuild/Tests/aether_doc_bench/out/spark_qwen_coder_30b_v6_numbered_smallonly.json`
- `/Users/mke/PBuild/Docs/aether_benchmark_analysis_note.md`

## What has gone wrong recently

### 1. Dataset content bug

Earlier 30B training runs were believed to be using the curated corpus, but the
active trainer was actually training only on instruction/repair JSONL and not
on the raw corpus in the intended way.

That was later corrected in the 30B path.

### 2. Training-format mismatch

The raw-corpus-only experiment changed not just the data content, but also the
effective supervision format. This made “corpus-only” benchmark conclusions
unsafe until the run format was reviewed carefully.

### 3. Local disk exhaustion on `claw2`

The training workspace on `/home/claw/training/...` lives on `/`, which is
space constrained. A merge/export step failed with:

- `RuntimeError: Failed saving - no disk space left!`

This is why export-only recovery to `/storage` was added.

### 4. Slow vLLM readiness on `/storage`

Serving merged safetensor checkpoints from `/storage` works, but startup is
slow because:

- `/storage` is NFS
- the checkpoint is large
- vLLM performs compile and warmup after weight load

### 5. Hostname resolution inconsistencies

This environment can SSH directly to:

- `claw@100.124.15.16`

It cannot currently resolve at least one other expected hostname:

- `claw1`

## Historical and planned flow

### Historical phases

1. local Aether/compiler/doc iteration
2. local benchmark harness against local and remote OpenAI-compatible endpoints
3. 4B-path scaffold for `Qwen3-4B-Base`
4. move to larger 30B Unsloth QLoRA path on `claw2`
5. repeated corpus / doc / benchmark adjustments
6. export and serve merged specialized checkpoints with vLLM

### Current practical flow

1. edit corpus/docs/scripts locally
2. prepare assets locally into `Tests/aether_specialization/out`
3. sync assets and trainer to `claw2`
4. run training in Unsloth container on `claw2`
5. if local export fails, recover via export-only pass to `/storage`
6. serve merged export with vLLM on `claw2`
7. benchmark locally against that endpoint using `Tools/aether_doc_bench.py`

### Planned or likely next steps

- stabilize the data format used for “native Aether” training
- separate:
  - raw-corpus prior learning
  - instruction-following SFT
  - repair fine-tuning
- keep a reproducible benchmark history for:
  - `full`
  - `small`
  - `none`
  - optional Python baseline
- decide whether the long-term small-model target remains `Qwen3-4B-Base` or
  moves to a better base model
- move more space-sensitive merge/export work to `/storage`

## Secrets and non-repo config

Do not commit secrets into the repo.

This includes:

- API keys
- Hugging Face tokens
- machine-local benchmark destination configs

Known local/private config locations or patterns:

- `Tests/aether_doc_bench/destinations.local.json`
- remote Hugging Face cache/token configuration under the user environment
- local LM Studio model inventory and auth settings

This document intentionally records locations and roles, not live secret
values.

## Recommended update rule

Whenever any of the following change, update this document:

- primary training host
- remote workspace path
- trainer script
- training corpus composition policy
- benchmark endpoint ports
- export destination path
- active base model
- benchmark harness contract

