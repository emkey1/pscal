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
- verified instruction JSONL
- verified repair JSONL

The output is intended to become the first input to a `Qwen/Qwen3-4B-Base`
specialization run.
