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
   silently ignores `options.num_ctx`.
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

Known past artifacts, for pattern-matching: glm-4.7-flash 0/30 (auth),
gemma-3n 0/8 (template), low-end guided 0/30s (fence-extract bug, ctx
overflow, evictions), task #29 stop-token empties, q36 empty content
(reasoning field + 8000-token cap), silent gen_ok=0 from too-small num_ctx.

## Scoring and provenance

- Old runs can be re-scored without GPUs from stored `source_code` +
  `tasks_v2_pos.json` fixtures.
- Beware benchmark contamination when scoring "none" (no-guide) conditions —
  see memory v2-benchmark-contamination; use the de-contaminated datasets.
- Update the findings doc
  (`components/aether/docs/aether_specialization_findings.md`) every few
  results, not at the end.
