# Aether Specialization Pipeline

This note defines the practical training path for a small model that should
emit canonical Aether natively.

The goal is not "teach a model about Aether." The goal is:

- first-try compile success
- first-try runtime success
- canonical style compliance
- low invented-import rate
- strong one-shot repair from compiler diagnostics

## Model target

Primary target:

- `Qwen/Qwen3-4B-Base`

Comparison / support models:

- `Qwen3-4B-Instruct-2507` for baseline comparison only
- optional coding-focused control such as `Qwen2.5-Coder`

Rationale:

- the base model is a better substrate for Aether specialization than the
  thinking/chat variant
- the instruct variant keeps spending tokens on reasoning and assistant-style
  behavior, which pollutes both benchmarking and specialization
- Aether needs compact raw code emission more than it needs general assistant
  behavior

## Core principle

Use a compiler-verified specialization pipeline, not a prose-only prompt or
"fine-tune on the guide and hope" pipeline.

The guide documents matter, but they should be a small support input, not the
main training signal. The model should see far more valid Aether source than
English text about Aether.

## Training stages

1. Build the evaluator.
2. Build the raw Aether corpus.
3. Build verified instruction-to-Aether pairs.
4. Build verified repair pairs.
5. Build preference pairs from plausible near-misses.
6. Run continued pretraining on raw Aether.
7. Run SFT on prompt-to-code pairs.
8. Run repair fine-tuning.
9. Run preference optimization.

## Evaluator requirements

Every candidate program should pass through:

1. parser/compiler
2. semantic checks
3. runtime test when the sample defines an expected stdout
4. canonical-style checks

The evaluator should explicitly score these Aether-specific rules:

- all `print(...)` and `println(...)` calls inside `fx { ... }`
- all task helpers and AI helpers inside `fx { ... }`
- `ret`, never `return`
- `type`, never `class`
- no invented imports
- no arithmetic on `ToonDoc` or `ToonNode`
- `toon_len(node)` for TOON arrays
- `length(arrayValue)` for dynamic arrays
- annotations above functions, never inside the body
- no Markdown fences when raw code is requested

## Dataset types

Build four datasets, not one.

### 1. Raw corpus

Purpose:

- continued pretraining
- language familiarity
- lexical regularity

Sources:

- `Examples/aether/base/*`
- `Examples/aether/showcase/*`
- hand-curated compiler-valid generated Aether

This corpus should be mostly code, not prose.

### 2. Instruction pairs

Purpose:

- natural language task -> raw canonical Aether

Each pair should include:

- a prompt
- a full solution
- expected stdout when applicable
- supporting files when applicable

### 3. Repair pairs

Purpose:

- diagnostic + broken code -> fixed code

High-value categories:

- missing `fx`
- `return` instead of `ret`
- invented imports
- `class` instead of `type`
- TOON root-shape mistakes
- tuple misuse
- misplaced contracts
- bad real formatting
- wrong array-length helper

### 4. Preference pairs

Purpose:

- choose the correct near-miss, not just reject obviously bad code

Typical preference categories:

- correct TOON root extraction vs iterating the wrong node
- canonical `let` vs redundant `let mut`
- verified module usage vs guessed import
- variadic `println(...)` vs mixed-type `+` concatenation

## Recommended benchmark metrics

Primary:

- `first_try_compile_rate`
- `first_try_runtime_pass_rate`
- `first_try_exact_stdout_rate`
- `canonical_style_pass_rate`
- `repair_after_one_diagnostic_rate`

Secondary:

- `no_invented_import_rate`
- `toon_root_shape_correct_rate`
- `no_markdown_fence_rate`
- `average_output_tokens`

## Spark host roles

Use the Spark box for two separate purposes:

1. baseline inference
2. specialization / fine-tuning

Do not assume the same serving stack is ideal for both.

Recommended baseline policy:

- benchmark the base model separately from instruct/thinking variants
- benchmark each candidate with the full guide, the small guide, and no guide
- keep a strict output contract
- record token usage, but do not let reasoning-heavy variants dominate your
  conclusions about the family

Recommended specialization policy:

- LoRA / QLoRA first
- keep the evaluator in the loop
- use rejection-sampling distillation only after verified examples exist

## Immediate next steps

1. harvest a compiler-verified raw corpus from examples
2. create a seed instruction-pair dataset
3. create a seed repair-pair dataset
4. benchmark `Qwen3-4B-Base` as the actual untuned baseline target
5. compare it against one instruct baseline, not many

## Current implementation status

The repository now includes the first local specialization pipeline pieces:

- `tools/aether_specialization_export_corpus.py`
- `tools/aether_specialization_build_dataset.py`
- `tools/aether_specialization_prepare_assets.py`
- `Tests/aether_specialization/seed_instruction_pairs.json`
- `Tests/aether_specialization/seed_repair_pairs.json`

The Spark baseline path is also scaffolded:

- `tools/qwen3_base_server.py`
- `tools/spark_qwen3_base_remote.py`
- `tools/run_qwen3_base_spark_benchmark.sh`

That path is meant to establish the untuned `Qwen/Qwen3-4B-Base` baseline for:

- Aether exact-stdout success
- Aether compile/runtime pass rates
- Python comparison pass rates
- token usage reported through the same benchmark harness
