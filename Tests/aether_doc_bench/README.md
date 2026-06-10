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

Optional pacing / cleanup fields:

- `after_each_command`: shell command run after every benchmark case
- `after_each_timeout_seconds`: timeout for that cleanup command
- `cooldown_seconds`: sleep after each benchmark case
- `request_timeout_seconds`: timeout for a single provider API request

Those fields are useful for local-model workflows where you want to unload a
model or let memory settle between runs.

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

## Interpreting repeated failures

If one task fails far more often than the others:

- it may indicate a documentation gap for that language surface
- it may indicate an Aether frontend/backend defect

The JSON report now records:

- every attempt per case
- whether the case was resolved after repair
- a compact failure fingerprint
- aggregate failure-pattern counts per document variant

That makes it easier to see whether the same effect-boundary, inference,
TOON-shape, or runtime issue keeps surfacing across multiple models.
