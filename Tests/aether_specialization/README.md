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

Imported external candidates are quarantined by default and do not enter the
training export unless promoted with `--include-in-training` or by directly
editing manifest metadata.

The separate reference corpus currently pulls from:

- `Docs/aether_for_llms_and_others.md`
- `Docs/aether_for_llms_with_small_contexts.md`

Keep that reference corpus separate from raw executable Aether source. It is
meant for instruction-style conditioning, synthetic-pair generation, or
retrieval-time context, not for blind mixing into the verified source corpus.

The output is intended to become the first input to a `Qwen/Qwen3-4B-Base`
specialization run.

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

Current default remote workspace:

- `~/training/aether-qwen3-base`
