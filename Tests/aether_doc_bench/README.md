# Aether Doc Benchmark

This directory defines a small benchmark corpus for measuring how well an LLM
can learn Aether from the current guide documents.

The main corpus now contains a couple dozen tasks ranging from tiny `hello
world` programs to larger agent-style TOON/reporting prompts that should push
models toward much longer answers.

## Purpose

The benchmark compares document variants such as:

- `Docs/aether_for_llms_and_others.md`
- `Docs/aether_for_llms_with_small_contexts.md`
- no guide at all (`--docs none`)

against the same set of programming tasks.

Success is measured pragmatically:

1. did the model return source code?
2. did the source compile with `build/bin/aether`?
3. did the program run successfully?
4. did stdout match the expected output exactly?

Optionally, the harness can also do repair iterations:

5. when a case fails, feed the failure back to the model
6. measure whether the model can recover on a later attempt

That makes the tool useful in two different ways:

- documentation refinement: repeated repair-needed patterns usually point to
  unclear or incomplete guidance
- Aether debugging: repeated failures on otherwise reasonable repaired code may
  indicate a compiler/runtime defect

## Runner

Use:

```bash
python3 Tools/aether_doc_bench.py --text-summary
```

Write a JSON report:

```bash
python3 Tools/aether_doc_bench.py \
  --output-json Tests/aether_doc_bench/out/latest.json \
  --text-summary
```

Run one task only:

```bash
python3 Tools/aether_doc_bench.py --task hello_fx --text-summary
```

Enable one repair attempt after an initial failure:

```bash
python3 Tools/aether_doc_bench.py \
  --task hello_fx \
  --repair-attempts 1 \
  --text-summary
```

Batch several Aether tasks behind one shared-guide prompt:

```bash
python3 Tools/aether_doc_bench.py \
  --destination command-template \
  --shared-guide-batch-size 4 \
  --text-summary
```

Run the no-guide baseline:

```bash
python3 Tools/aether_doc_bench.py \
  --docs none \
  --text-summary
```

List configured destinations:

```bash
python3 Tools/aether_doc_bench.py --list-destinations
```

## Providers

The harness now prefers named destination profiles from:

- `Tests/aether_doc_bench/destinations.template.json`
- optional local overrides such as `Tests/aether_doc_bench/destinations.local.json`

Each destination has an `id` and a `type`.

Current destination types:

- `openai_responses`
- `openai_chat_completions`
- `command`

`--shared-guide-batch-size` affects only the Aether initial-generation lane.
When set above `1`, the harness sends one prompt containing the selected guide
plus multiple tasks, expects one JSON reply with one program per task, then
splits prompt/usage accounting back across those cases. Repairs still run
per failed case. The Python baseline remains per-case because it has no guide
overhead to amortize.

Small models are forced back to per-task mode even when a larger batch size is
requested. Today the harness treats models at `8B` and below as small for this
purpose, because multi-program JSON batching is a poor fit for weak code models.

Optional pacing / cleanup fields:

- `after_each_command`: shell command run after every benchmark case
- `after_each_timeout_seconds`: timeout for that cleanup command
- `cooldown_seconds`: sleep after each benchmark case
- `request_timeout_seconds`: timeout for a single provider API request
- `request_max_retries`: retry transient HTTP/API failures this many times
- `retry_backoff_seconds`: base delay for exponential backoff between retries

Those fields are useful for local-model workflows where you want to unload a
model or let memory settle between runs.
They are also useful for hosted APIs that enforce rate limits, such as Gemini.

### OpenAI Responses

Set `OPENAI_API_KEY`, then run:

```bash
python3 Tools/aether_doc_bench.py \
  --destination openai-gpt-5-mini \
  --text-summary
```

### OpenAI-compatible `/chat/completions`

Add a destination like this:

```json
{
  "id": "my-local-model",
  "type": "openai_chat_completions",
  "base_url": "http://host:port/v1",
  "api_key": "",
  "model": "provider/model-name",
  "temperature": 0.2,
  "max_output_tokens": 3000,
  "request_timeout_seconds": 120,
  "request_max_retries": 4,
  "retry_backoff_seconds": 5,
  "after_each_command": "",
  "after_each_timeout_seconds": 60,
  "cooldown_seconds": 2
}
```

Then run:

```bash
python3 Tools/aether_doc_bench.py \
  --destinations-config Tests/aether_doc_bench/destinations.local.json \
  --destination my-local-model \
  --text-summary
```

### External command

For other LLM workflows, point the harness at a command that consumes a prompt
file and prints raw Aether source to stdout via a `command` destination.

The command receives a `{prompt_file}` placeholder.

Example shape:

```json
{
  "id": "my-command-runner",
  "type": "command",
  "command_template": "my-llm-runner --prompt-file {prompt_file}"
}
```

### Deterministic self-test

This repository also includes a fake model so the harness itself can be tested
without any network calls:

```bash
python3 Tools/aether_doc_bench.py \
  --tasks Tests/aether_doc_bench/smoke_tasks.json \
  --destination command-template \
  --text-summary
```

Batch-mode self-test:

```bash
python3 Tools/aether_doc_bench.py \
  --tasks Tests/aether_doc_bench/smoke_tasks.json \
  --destination command-template \
  --shared-guide-batch-size 2 \
  --text-summary
```

There is also a repair-path self-test:

```bash
python3 Tools/aether_doc_bench.py \
  --tasks Tests/aether_doc_bench/smoke_tasks.json \
  --destinations-config Tests/aether_doc_bench/repair_test_destinations.json \
  --destination command-repair-template \
  --task hello_fx \
  --repair-attempts 1 \
  --text-summary
```

## Local config

Keep machine-specific or private model settings in:

- `Tests/aether_doc_bench/destinations.local.json`

That local file should not be committed.

For example, a local OpenAI-compatible endpoint with no API key:

```json
{
  "destinations": [
    {
      "id": "c1t-gpt-oss-120b",
      "type": "openai_chat_completions",
      "base_url": "http://c1t:8001/v1",
      "api_key": "",
      "model": "openai/gpt-oss-120b",
      "temperature": 0.2,
      "max_output_tokens": 3000
    }
  ]
}
```

If your local runtime needs help managing memory, add:

```json
"request_timeout_seconds": 180,
"after_each_command": "your-unload-command-here",
"cooldown_seconds": 2
```

If your hosted provider rate-limits aggressively, add retry/backoff as well:

```json
"request_timeout_seconds": 180,
"request_max_retries": 4,
"retry_backoff_seconds": 5,
"cooldown_seconds": 3
```

For smaller local models, it is often better to treat them as a
small-context core lane rather than forcing them through the larger guide.
In practice that means:

- run them against `Docs/aether_for_llms_with_small_contexts.md`
- keep only one local model loaded at a time
- unload a model before loading the next one
- count stability as part of the result, not just final exact-match rate

## Task manifest

Tasks live in `tasks.json`.

Each task currently defines:

- `id`
- `title`
- `prompt`
- `expected_stdout`
- optional `timeout_seconds`
- optional `cwd`
- optional `files`

Keep tasks small, deterministic, and exact-output based.

The goal is not to test every language feature here. The goal is to measure
how reliably a model can turn the guide into working Aether.

## Report fields

Every variant report includes the original per-case results plus aggregate
rollups. When batch mode is enabled it also includes:

- `shared_guide_batch_size`
- `batch_mode_enabled`
- `batch_runs`

Each `batch_runs` entry records the grouped task ids, shared prompt token
estimate, and shared provider-usage block for that batch request.

## Interpreting repeated failures

If one task fails far more often than the others:

- it may indicate a documentation gap for that language surface
- it may indicate an Aether frontend/backend defect

The JSON report now records:

- every attempt per case
- whether the case was resolved after repair
- a compact failure fingerprint
- aggregate failure-pattern counts per document variant
- normalized provider token usage per attempt when the endpoint returns it
- aggregate token-usage totals per document variant
- a `doc_token_reference` block with both long and short guide sizes for each run
- a `doc_token_reference` entry for the no-guide baseline (`none`), which is size `0`

When Aether exposes a structured diagnostic `code`, the benchmark prefers that
stable identifier over raw stderr text. This means recurring failures collapse
into buckets like `run_error_code:AETH-EFFECT-FX-REQUIRED` instead of being
split apart by line numbers or wording drift.

That makes it easier to see whether the same effect-boundary, inference,
TOON-shape, or runtime issue keeps surfacing across multiple models.

For OpenAI-compatible providers that return a `usage` object, each attempt now
includes a normalized `usage` block with:

- `prompt_tokens`
- `completion_tokens`
- `total_tokens`
- `cached_tokens` when available
- `reasoning_tokens` when available
- `provider_raw` for the original provider payload

Each doc variant also includes a `usage_summary` rollup so you can compare the
token cost of the long and short guides in addition to correctness.

To separate language compactness from retry overhead, the report now includes
three different token views:

- `usage_summary`: total workflow tokens across all attempts
- `final_source_token_summary`: final answer size only, one program per case
- `exact_final_source_token_summary`: final answer size only for exact-match cases

Matching usage rollups also exist for final attempts:

- `final_usage_summary`
- `run_ok_final_usage_summary`
- `exact_final_usage_summary`

There is also an optional Python comparison lane:

```bash
python3 Tools/aether_doc_bench.py --python-baseline --text-summary
```

When enabled, each case also asks the same model for a Python 3 solution,
executes it locally with `python3`, and records:

- per-attempt normalized usage
- per-attempt approximate answer tokens
- exact-output success/failure
- per-variant Python usage and answer-token rollups

This is intended to make Aether-vs-Python token comparisons straightforward.
