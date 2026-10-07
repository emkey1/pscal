# Aether Doc Benchmark

This directory defines a small benchmark corpus for measuring how well an LLM
can learn Aether from the current guide documents.

The main corpus now contains a couple dozen tasks ranging from tiny `hello
world` programs to larger agent-style TOON/reporting prompts that should push
models toward much longer answers.

## Purpose

The benchmark compares document variants such as:

- `components/aether/docs/aether_for_llms_and_others.md`
- `components/aether/docs/aether_for_llms_with_small_contexts.md`
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
python3 tools/aether_doc_bench.py --text-summary
```

Write a JSON report:

```bash
python3 tools/aether_doc_bench.py \
  --output-json Tests/aether_doc_bench/out/latest.json \
  --text-summary
```

Run one task only:

```bash
python3 tools/aether_doc_bench.py --task hello_fx --text-summary
```

Enable one repair attempt after an initial failure:

```bash
python3 tools/aether_doc_bench.py \
  --task hello_fx \
  --repair-attempts 1 \
  --text-summary
```

Batch several Aether tasks behind one shared-guide prompt:

```bash
python3 tools/aether_doc_bench.py \
  --destination command-template \
  --shared-guide-batch-size 4 \
  --text-summary
```

Run the no-guide baseline:

```bash
python3 tools/aether_doc_bench.py \
  --docs none \
  --text-summary
```

List configured destinations:

```bash
python3 tools/aether_doc_bench.py --list-destinations
```

## Reproducibility: binary, guides, seeds

Every report names exactly what produced it, and the harness refuses to start
when it cannot:

- **Binary.** At start-up the harness copies `--aether-bin` into a private
  temp dir, hashes it, and runs every case on that copy, so a rebuild mid-run
  cannot mix compilers. Each run records `binary_sha256`.
- **Skew guard.** The run stops if the binary was built from a dirty tree
  (`-dirty`), if `components/aether` is not at the umbrella's gitlink (`+` in
  `git submodule status`), or if the binary's `+sha` is not `--aether-root`'s
  HEAD. A standalone build (no `+sha`) always needs `--allow-skew` together with
  `--aether-bin-sha256 <its sha256>`. `--allow-skew` is recorded with its
  reasons under `skew_guard`.
- **Guides.** `--doc NAME=PATH` (repeatable) defines or replaces a variant;
  `--aether-root DIR` moves the default `docs/`. Each variant records its path,
  stamp and sha256 (`guides`, `doc_sha256`). For a board, point `--doc` (or
  `--aether-root`) at the `docs/` of the same `~/aether-<sha>` checkout that
  built the binary.
- **Tasks and destinations.** `tasks_sha256`, `tasks_version` and
  `destinations_sha256`; the umbrella HEAD and dirty flag; and the harness's
  own sha plus a sha256 of every prompt template (`harness.prompt_template`),
  which is what identifies a harness change now that harness-only guide-stamp
  bumps are retired.
- **Seeds.** A destination's top-level `seed` is its base; `--seed-base N`
  sets one for every self-hosted destination without its own. Repeat `r` sends
  `seed = base + r` (the protocol is T=0.2, seed 42+r, 3 repeats locally; cloud
  x1). `--start-repeat R --repeats 1` re-runs exactly repeat R. A destination
  key the harness does not read is now an error, not silently ignored.
- **Variant order.** Variants run back to back per task, in an order that
  rotates by one each task, instead of variant-major; `case_sequence` records
  the order cases actually ran in.
- **Extra compiler flags.** `--aether-arg=FLAG` (repeatable) goes in front of
  the program on every aether call and is recorded, for language A/Bs.

- **Context fit.** Every request is guarded on measured prompt tokens +
  the output budget + a 512-token margin (`--context-margin`) <= the context
  window. Tokens are counted with the server's own `/tokenize` (llama.cpp,
  vLLM) on self-hosted http endpoints, else tiktoken o200k when importable,
  else chars/3.4; `context_fit.prompt_tokens_method` says which. The output
  budget is clamped to what is left (`context_fit.clamped`,
  `max_tokens_sent`); below `--min-output-tokens` (1024) the request is not
  sent and the attempt records `not_sent: context_overflow`. The window comes
  from `prompt_context_limit`, else LM Studio / vLLM / llama.cpp / T'Ra
  `/api/targets`. A self-hosted or T'Ra destination whose window is neither
  configured nor detectable is refused at start-up unless
  `--allow-unknown-context`. A >5% gap between the measured prompt and the
  provider's `usage.prompt_tokens` is recorded (`prompt_token_gap`) and warned
  about. `doc_approx_tokens` (chars/4) stays for comparability next to
  `doc_tokens_o200k`.

`--preflight-only` runs these checks and prints what it found. The drivers
(`bench_one_destination.sh`, `bench_one_destination_cfg.sh`,
`run_cloud_spectrum.sh`) call it once before their suite loop, take guide
overrides from `DOC="NAME=PATH ..."` and pass `BENCH_ARGS` through to the
harness.

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
python3 tools/aether_doc_bench.py \
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
python3 tools/aether_doc_bench.py \
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
python3 tools/aether_doc_bench.py \
  --tasks Tests/aether_doc_bench/smoke_tasks.json \
  --destination command-template \
  --text-summary
```

Batch-mode self-test:

```bash
python3 tools/aether_doc_bench.py \
  --tasks Tests/aether_doc_bench/smoke_tasks.json \
  --destination command-template \
  --shared-guide-batch-size 2 \
  --text-summary
```

There is also a repair-path self-test:

```bash
python3 tools/aether_doc_bench.py \
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

- run them against `components/aether/docs/aether_for_llms_with_small_contexts.md`
- keep only one local model loaded at a time
- unload a model before loading the next one
- count stability as part of the result, not just final exact-match rate

## Task manifest

Tasks live in one manifest per suite. The boards use `tasks_v2_pos.json`
(simple, the default), `tasks_hard_v2.json`, `tasks_hard_nontoon.json`,
`tasks_cs.json` and the three `tasks_frontier*.json` suites. `tasks.json` is
the original v1 set, off the boards and kept for history. Check every
reference solution, negative and the sandbox probe against a binary with
`python3 tools/aether_oracle_check.py --aether-bin <bin>`; the harness runs the
reference and sandbox checks for the tasks it is about to score as a
pre-flight and stops on a failure unless `--allow-broken-oracle`.

Each task currently defines:

- `id`
- `title`
- `prompt`
- `expected_stdout`
- optional `timeout_seconds`
- optional `cwd`
- optional `files`
- optional `reference_solution` (checked by `tools/aether_oracle_check.py`)
- optional `expected_returncode` (default 0). A case is exact only when the
  exit status AND stdout match. Note that `halt` is a proc-class effect, so a
  task that needs it must run with `--sandbox-deny net`, not the default
  `net,proc`.
- optional `stdin`: a string, or `{"file": NAME}` naming one of `files`, fed to
  the program; without it the program reads EOF.
- optional `hide_expected_stdout` (also settable for the whole suite at the
  top level of the manifest). The first-attempt prompt then omits the
  `Expected stdout` block, so the task prompt must specify the output format
  completely; repair rounds still show it (D37a). Only trap suites use it, so
  every existing suite keeps its prompts byte for byte.

Entries with `should_fail: true` are negative-tier compiler invariants (a fixed
`program` plus `expected_error_code`), not tasks; the harness skips them.

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

## Headline metrics and infra failures

Each variant `summary` carries, besides the old counts:

- `first_attempt_exact` / `fa_rate` / `fa_ci95` (FA) and `final_exact` /
  `fx_rate` / `fx_ci95` (FX), with Wilson 95% intervals over cases (repeats of
  a task are not independent, so read them as a lower bound on uncertainty);
- `first_attempt_classes`: the thesis failure classes of each first attempt,
  worst first -- `silent_wrong` (exited as a success, wrong stdout),
  `crash_hang` (timeout, signal, rc >= 128), `uncoded_error` (no `CODE-NNN`),
  `coded_error` -- plus `pass`, `infra_failed` and `not_sent`;
- `per_task`, `task_majority_fa/fx` and the `flaky_fa/fx` task sets.

A case whose generation measured nothing (HTTP 4xx/5xx, 402/429/quota,
`RESOURCE_EXHAUSTED`, transport, a provider deadline, an empty reply) is
tagged `infra_failed` with an `infra_kind`. It stays in the denominator, the
text summary prints `HEADLINE WITHHELD`, and the harness exits **3** after
writing the report. Re-run those cases with
`Tests/aether_doc_bench/rerun_nogen_cases.py --report REPORT --apply`, which
reads everything it needs (binary and its sha256, guide, seed base, repeat)
from the report; the drivers do this automatically. Each attempt also stores
the provider's `finish_reason`. `--resummarize REPORT` recomputes an old
report's summaries; `summarize_full_vs_medium.py` adds a paired bootstrap
non-inferiority test (`--margin`, default 3 points).

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
python3 tools/aether_doc_bench.py --python-baseline --text-summary
```

When enabled, each case also asks the same model for a Python 3 solution,
executes it locally with `python3`, and records:

- per-attempt normalized usage
- per-attempt approximate answer tokens
- exact-output success/failure
- per-variant Python usage and answer-token rollups

This is intended to make Aether-vs-Python token comparisons straightforward.
