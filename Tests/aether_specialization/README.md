# Aether Specialization Seeds

This directory holds the first compiler-verified seed data for Aether model
specialization.

## Files

- `seed_instruction_pairs.json`
  Natural-language prompt -> canonical Aether program pairs.
- `seed_repair_pairs.json`
  compiler/runtime diagnostic + broken source -> fixed canonical Aether pairs.

## Design rules

- Every `solution` or `fixed_source` should compile with `build/bin/aether`.
- If `expected_stdout` is present, it should match exactly.
- Keep seeds small and high signal. These are not meant to be the full corpus.
- Prefer canonical Aether over permissive forms when both compile.

## Intended use

These manifests feed `tools/aether_specialization_build_dataset.py`, which
verifies each sample before writing JSONL for later SFT or repair training.
The seed manifests are no longer the whole supervised set: canonical
compiler-verified corpus candidates with meaningful stdout are also promoted
into instruction-style SFT records automatically.

Typical local flow:

```bash
python3 tools/aether_specialization_prepare_assets.py \
  --output-dir /tmp/aether-specialization-assets
```

That command exports:

- a raw-code corpus manifest
- a reference/instruction corpus manifest
- verified instruction JSONL
- verified repair JSONL

The raw corpus currently pulls from:

- `Examples/aether/base/*`
- `Examples/aether/showcase/*`
- manifest-approved entries from
  `Tests/aether_specialization/corpus_candidates_manifest.json`

Raw corpus export can also carry lightweight per-example metadata from
`Tests/aether_specialization/corpus_candidates_manifest.json`. Intended keys:

- `canonical`: `true` when the sample is preferred new-style Aether
- `fixture_required`: `true` when the example depends on extra files
- `assumption_bearing`: `true` when expected output depends on a stated
  assumption rather than a fully-specified prompt
- `tags`: short labels such as `module`, `toon`, `report`, `tuple`, `formatting`
- `notes`: one-line rationale or caveat

These fields are advisory training metadata. They do not replace compiler
verification, but they help keep canonical examples distinct from compatibility
or assumption-heavy ones.

Additional source-of-truth rules:

- `Tests/aether_specialization/corpus_candidates_manifest.json` is the source
  of truth for repo corpus candidates used in training export
- a file under `corpus_candidates/` that is not listed in the manifest is a
  validation failure
- a manifest entry whose file is missing is a validation failure
- JSON fixtures belong under `Tests/aether_specialization/fixtures/`, not under
  `corpus_candidates/`

Validate corpus structure explicitly:

```bash
python3 Tools/aether_specialization_validate_corpus.py --strict
```

`corpus_candidates/` contains only corpus entries — every file is listed in
the manifest with `canonical: true` and has a meaningful descriptive name.
Exploratory probes, scratch outputs, and duplicate drafts live in `scratch/`
and are not referenced by the manifest or the training export pipeline.

The separate reference corpus currently pulls from:

- `Docs/aether_for_llms_with_small_contexts.md`

By default, only the small-context guide is exported into the reference corpus.
Use `tools/aether_specialization_export_reference_corpus.py --include-full-guide`
when a run explicitly needs the full guide as reference ballast.

Keep that reference corpus separate from raw executable Aether source. It is
meant for instruction-style conditioning, synthetic-pair generation, or
retrieval-time context, not for blind mixing into the verified source corpus.

The output is intended to feed specialization runs. The original baseline path
used `Qwen/Qwen3-4B-Base`; the current larger-model path targets
`unsloth/Qwen3-Coder-30B-A3B-Instruct` inside the Unsloth DGX Spark container.

## Spark workflow

Prepare and sync verified assets to the Spark workspace:

```bash
python3 tools/aether_specialization_sync_to_spark.py
```

Sync the trainer script only:

```bash
python3 tools/spark_qwen3_base_train_remote.py sync-script
```

Start a short smoke run:

```bash
python3 tools/spark_qwen3_base_train_remote.py \
  --run-name sft-seed-smoke-v1 \
  --epochs 1 \
  --max-seq-len 4096 \
  --save-steps 5 \
  start
```

Check status and tail the remote log:

```bash
python3 tools/spark_qwen3_base_train_remote.py --run-name sft-seed-smoke-v1 status
```

Start the convenience wrapper:

```bash
bash Tools/run_qwen3_base_spark_training.sh sft-seed-v1
```

Current first-pass trainer:

- `tools/qwen3_base_lora_sft.py`

Current remote helpers:

- `tools/aether_specialization_sync_to_spark.py`
- `tools/spark_qwen3_base_train_remote.py`

## Unsloth 30B Spark workflow

The current large-model path uses the upstream Unsloth DGX Spark container
recipe (`Dockerfile_DGX_Spark`) instead of a bare-metal Python environment on
the Spark host.

Prepare and sync verified assets plus the Unsloth trainer:

```bash
python3 tools/spark_unsloth_qwen_coder_train_remote.py sync
```

Build the pinned Unsloth DGX Spark image on the remote host:

```bash
python3 tools/spark_unsloth_qwen_coder_train_remote.py build-image
```

Start a run:

```bash
python3 tools/spark_unsloth_qwen_coder_train_remote.py \
  --run-name sft-qwen-coder-30b-v1 \
  --epochs 8 \
  --eval-cases 12 \
  start
```

Check status and tail the remote log:

```bash
python3 tools/spark_unsloth_qwen_coder_train_remote.py \
  --run-name sft-qwen-coder-30b-v1 \
  status
```

Stop the run container:

```bash
python3 tools/spark_unsloth_qwen_coder_train_remote.py \
  --run-name sft-qwen-coder-30b-v1 \
  stop
```

Design notes for the Unsloth path:

- model: `unsloth/Qwen3-Coder-30B-A3B-Instruct`
- method: QLoRA with `load_in_4bit=True`
- LoRA targets:
  `q_proj,k_proj,v_proj,o_proj,gate_proj,up_proj,down_proj`
- LoRA config: `r=16`, `alpha=32`
- gradient checkpointing: `"unsloth"`
- compute: bf16
- router/gating fine-tuning remains off
- validation holdout defaults to 12 compiler-verified cases
- `max_seq_length` is auto-sized just above the longest tokenized training
  example unless explicitly overridden
- GGUF export defaults to non-Q4 variants only

Convenience wrapper:

- `tools/run_unsloth_qwen_coder_30b_spark_training.sh`

Current default remote workspace:

- `~/training/aether-qwen3-base`
