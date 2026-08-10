# Aether benchmark / eval ops runbook

One place for the failure fingerprints that have each cost hours-to-days.
Read before any bench or eval sweep (`tools/aether_doc_bench.py`,
`tools/aether_wild_eval.py`, the idea miner, or ad-hoc model evals).
Companion policies live in CLAUDE.md (budgets, triage, long runs, T'Ra).

## Pre-flight checklist

1. **Binary is current.** Bench must run the pushed SHA: locally use an
   absolute `--aether-bin` (the harness cwd is a temp dir); on claws the
   canonical binary is `~/aether-current/build/bin/aether`, refreshed by
   `refresh_aether.sh` / the autodeploy hook. Verify `aether --version`
   matches `components/aether/VERSION` on every node involved. Results are
   stamped with `aether_version`; a mismatch invalidates the run.
2. **Route through T'Ra** (http://100.121.116.25:8793). `GET /api/targets`
   first — never guess target names. One job per endpoint at a time; parallel
   across endpoints is good. Direct hits on Ollama/LM Studio during shared use
   cause silent contention (models spill to CPU, "slow model" mysteries).
3. **Budgets large.** Context 20k–32k minimum (128k where it fits), generous
   max output tokens. Reasoning models put thinking in a separate field and
   return EMPTY `content` if the output budget is tight. Ollama `/v1`
   silently ignores `options.num_ctx`. Concretely: the older Gemini configs
   carry `max_output_tokens: 3000`, which truncates on any task needing 50–90
   lines of Aether plus thinking, and **truncation scores as a model failure**.
   16000 is a sane floor for the frontier suites.
4. **Stop tokens off for reasoning models.** The harness stop marker
   (`__AETHER_BENCH_END__`) can fire inside the thinking phase → empty
   content → bogus 0s. Use `extra_body stop:null` for reasoning models.
5. **Tokens/auth.** Locations in CLAUDE.md (read-only, never write). GLM
   proxy JWT expires — on 401, re-copy from openclaw.json. A dummy/expired
   LM Studio key 401s silently and produces 0-case runs.
6. **Endpoints warm.** Ollama keep-alive should be 1h (default 5 min causes
   cold-load timeouts). Check nothing stale is squatting on GPU RAM
   (`ollama ps`, unload idle models). claw2 docker Ollama is **:11435**.
7. **Detached + resumable.** Sweeps run in tmux (+caffeinate locally), with
   per-unit result files and skip-finished logic. Never a foreground Bash
   call (2-minute timeout kills long LLM calls with exit 143). Smoke-test one
   case and inspect the actual result artifact before launching the sweep.
8. **Confirm the model still exists.** A retired model 404s from the
   OpenAI-compat endpoint and produces a clean-looking 0/N that measures
   nothing (`gemini-2.0-flash-lite`, 2026-08-10). Check before, not after.

## Post-flight: the bogus-score triage tree

Check every score as it lands. A surprising number (0/30 from a capable
model, a sudden cliff, all-identical outputs) is an artifact until proven
otherwise. Triage IMMEDIATELY, in this order:

1. **generated_ok / gen_ok counts.** Low gen_ok = harness/serving problem,
   not model weakness. (Generated-fully-but-wrong = plausibly real.)
2. **Return codes.** 401 (token), timeouts (cold load, keep-alive), refusals.
3. **stderr histogram** across cases — one repeated error explains everything.
4. **Sample the generations.** Look for: empty content (stop-token or output
   budget), truncation mid-program (context overflow), fence-extraction
   misses, template incompatibility, the model answering in the wrong
   language entirely.
5. Fix, then **rerun the affected cells**. Never publish a table with known
   load-failed or artifact rows; either rerun or drop with a note.

**Identical wrong output across vendors means the prompt, not the models.** The
cheapest tell in the whole harness. If two independently-trained models emit the
same wrong bytes, stop investigating the models and go read the task. In the
`toon_fleet_rollup` case (2026-08-10) eight models across three vendors made the
same error and four emitted byte-identical wrong output; the task was
under-specified. Convergence that tight is never a capability signal.

**A partial report's top-level `summary` lies.** It records what the run was
configured to do, not what it finished. A hung 2026-07-05 Ornith run reported
`total_cases_per_destination: 19, doc_variants: 2` while holding 14 cases and one
variant. Check `len(destinations[].variants)` and each
`variants[].summary.total_cases` before trusting any rollup.

Known past artifacts, for pattern-matching: glm-4.7-flash 0/30 (auth),
gemma-3n 0/8 (template), low-end guided 0/30s (fence-extract bug, ctx
overflow, evictions), task #29 stop-token empties, q36 empty content
(reasoning field + 8000-token cap), silent gen_ok=0 from too-small num_ctx.

## Measurement design

- **The frontier signal is probabilistic, not pass/fail.** A capable model can
  score 36/36 exact and still retry on ~11% of attempts. Run the discriminating
  suites with `--repeats 3` and **report retry rate as a column**, or that
  signal is invisible. DeepSeek-V4-Flash looked identical to a clean run until
  repeats exposed three tasks it stumbled on 1-in-3 to 2-in-3 of the time.
- **Retry count discriminates where the score does not.** Two models both at
  12/12 differed by 2 repair rounds vs 0. First-pass quality separates models
  that final scores rank identically.
- **Single-run, single-task deltas are noise.** Re-running moved one model 11→12
  and another 7→6 on the same suite; two complete repeats of the 2026-07-05
  Ornith cs suite disagreed by one task in each direction. Only multi-task gaps
  are real. Do not write a finding on a one-task move.
- **`--repeats` only works on direct destinations.** Through T'Ra the
  idempotency key returns one cached job N times instead of N samples. Fixed
  harness-side in pscal `b80a7d6b9`, but T'Ra's payload cannot yet exploit it.
- **A saturating suite is still worth running.** The surface suite sits at
  14–15/15 everywhere because the guide is in the prompt, so it tests recall,
  not capability. Its value is catching compiler regressions, which it has
  already done.

## Task authoring

- **`task.files` is never shown to the model.** `build_prompt` materializes
  files on disk for the *run* but does not put their contents in the prompt. A
  task providing a file must state its schema in prose, or the model is guessing
  — and a guess that fails is indistinguishable from a capability failure. Either
  inline the file (the `tasks_hard` convention) or name every field and its
  nesting. 37 of 191 tasks provide files; audit before trusting one.
- **Give every task a reference solution whose output was captured by running
  it**, so `expected_stdout` is provably achievable and byte-exact.

## Scoring and provenance

- Old runs can be re-scored without GPUs from stored `source_code` +
  `tasks_v2_pos.json` fixtures.
- Beware benchmark contamination when scoring "none" (no-guide) conditions —
  see memory v2-benchmark-contamination; use the de-contaminated datasets.
- Update the findings doc
  (`components/aether/docs/aether_specialization_findings.md`) every few
  results, not at the end.
