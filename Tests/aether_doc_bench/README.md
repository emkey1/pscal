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

## Providers

### OpenAI

Set `OPENAI_API_KEY`, then run:

```bash
python3 Tools/aether_doc_bench.py \
  --provider openai \
  --model gpt-5-mini \
  --text-summary
```

### External command

For other LLM workflows, point the harness at a command that consumes a prompt
file and prints raw Aether source to stdout.

The command receives a `{prompt_file}` placeholder.

Example shape:

```bash
python3 Tools/aether_doc_bench.py \
  --provider command \
  --command-template 'my-llm-runner --prompt-file {prompt_file}' \
  --text-summary
```

### Deterministic self-test

This repository also includes a fake model so the harness itself can be tested
without any network calls:

```bash
python3 Tools/aether_doc_bench.py \
  --provider command \
  --command-template 'python3 Tests/aether_doc_bench/mock_model.py {prompt_file}' \
  --text-summary
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
