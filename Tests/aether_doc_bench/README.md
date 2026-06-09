# Aether Doc Benchmark

This directory defines a small benchmark corpus for measuring how well an LLM
can learn Aether from the current guide documents.

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
  "max_output_tokens": 3000
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
  --destination command-template \
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
