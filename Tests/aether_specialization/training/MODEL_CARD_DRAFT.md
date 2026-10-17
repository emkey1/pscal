---
license: apache-2.0
base_model: Qwen/Qwen3.8-27B
language:
  - en
tags:
  - code
  - aether
  - lora
  - fine-tuned
library_name: transformers
pipeline_tag: text-generation
---

<!-- DRAFT 2026-10-17. Numbers marked TBD come from the run chosen for release (internal notes:
Tests/aether_specialization/training_runs/*/RESULTS.md). Not published. -->

# Qwen3.8-27B-Aether (no-guide)

A LoRA fine-tune of [Qwen/Qwen3.8-27B](https://huggingface.co/Qwen/Qwen3.8-27B) that writes
**Aether** programs without a language guide in the prompt.

[Aether](https://github.com/emkey1/aether) is a small, statically typed language with explicit effects
(`fx { }` blocks), contracts (`@pre` / `@post`), records with methods, arrays and 2-D arrays, and a built-in
JSON/TOON API. It is new, so general models do not know it: the untrained base model passes **0 of 73**
benchmark tasks on the first attempt without a guide. With a ~15K-token guide in every prompt, strong models
do well; this fine-tune moves that knowledge into the weights.

## Results

All scores: no guide in the prompt, temperature 0, seed 42, one sample per task, up to two repair rounds
in which the model sees the compiler's diagnostic and the observed output. "First attempt" counts tasks
solved by the first program; "after repair" counts tasks solved within the repairs. Compiler: Aether
language TBD.

| suite | tasks | untrained base | **this model** | base model + medium guide |
|---|---:|---:|---:|---:|
| core (easy) | 35 | 0 | TBD | TBD |
| hard (TOON data processing) | 14 | 0 | TBD | TBD |
| hard (non-TOON) | 5 | 0 | TBD | TBD |
| algorithms | 19 | 0 | TBD | TBD |
| **held-out** frontier (surface) | 15 | — | TBD | — |
| **held-out** frontier (algorithmic) | 14 | — | TBD | — |
| **held-out** frontier (specification) | 12 | — | TBD | — |
| **held-out** TOON / MStream | 12 | — | TBD | — |

The held-out suites were never used to choose or write training data; they measure whether the gains carry
beyond the tasks the training data was developed against.

**Python, same model, same tasks** (for scale): the untrained base writes Python for TBD of 73 core tasks
(TBD of 53 held-out) on the first attempt; this fine-tune TBD / TBD, i.e. fine-tuning on Aether did not
TBD its Python. Some tasks require Aether-only features (a `has_toon()` check, contracts, `fx`), so the
Python comparison is indicative; excluding those tasks: TBD.

Single training seed. Expect a task or two of difference between training runs.

## How to use

The model was trained with thinking **off**. Serve it so the chat template renders the same way:

- **vLLM / transformers**: pass `chat_template_kwargs={"enable_thinking": false}`.
- **Ollama** (OpenAI-compatible `/v1`): send `"reasoning_effort": "none"`. A top-level `think: false` is
  ignored on that endpoint and the model will reason first. Set `num_ctx` in a Modelfile
  (`PARAMETER num_ctx 32768`); `/v1` ignores per-request context options.

Prompt format used in training and evaluation (single user message):

```
You are writing Aether code.

Do not assume you have an Aether reference guide for this task.
Use only what you already know about the language and infer cautiously.

Write exactly one complete Aether program that solves the task below.

Requirements:
- Return only raw Aether source code.
...
Task:
<task statement>

Expected stdout:
<exact expected output>

Aether source:
```

Shorter natural-language requests ("Write an Aether program that ...") also work; part of the training data
is in that form.

## Training

- **Method**: LoRA r=32, alpha=64, dropout 0, on attention and MLP projections; base loaded in 4-bit
  (bitsandbytes); lr 1e-4, batch 1 x 8 accumulation, early stopping on a held-out set of 12 records (stopped
  at epoch TBD); responses only (prompt tokens masked). Unsloth + TRL + PEFT on one NVIDIA GB10 (DGX Spark),
  about 30-45 minutes.
- **Data** (TBD records; all programs compile and print their stated output on the Aether compiler, checked
  on every build):
  - a curated corpus of Aether programs and natural-language task/solution pairs;
  - repair drills: a broken program, the compiler's diagnostic, the fix;
  - targeted packs for the APIs and idioms the earlier runs got wrong (JSON/TOON, memory streams, 2-D
    tables, array value semantics, stepped loops);
  - **distilled pairs**: task statements and independent Python solutions written by
    [MiMo-V2.6-Flash-RL](https://huggingface.co/XiaomiMiMo/MiMo-V2.6-Flash-RL) (MIT), Aether solutions written by
    the untrained base model *with* the guide, kept only where the Aether program prints exactly what the
    Python program prints. All generation requests were audited to come from MiMo.
- **Decontamination**: every training example is checked against every benchmark task (identical expected
  output is dropped; 5-gram similarity to any reference solution must stay below 0.5), and the held-out suites
  are excluded from data authoring.

The language, its compiler and the guides are at [emkey1/aether](https://github.com/emkey1/aether). The
training data, the benchmark tasks and the scoring reports are published as a companion dataset (TBD:
Hugging Face dataset name).

## Limitations

- Aether evolves; the model knows the language as of version TBD. Features added later will be unknown to it.
- It writes small, self-contained programs well; it has not been evaluated on multi-file projects or long
  programs.
- First attempts still fail on some idioms that one repair round fixes (e.g. file parsing outside an `fx`
  block); use it with the compiler in the loop where possible.
- Single training seed; benchmark differences of one or two tasks are within run-to-run variation.

## License and attribution

Apache-2.0, as the base model. Distilled training data was generated with MiMo-V2.6-Flash-RL (MIT, Xiaomi).
