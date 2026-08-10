# Frontier suite results — 2026-08-10

Three new suites, run against four models. `aether 2026-08-09-1`, medium guide,
`--repair-attempts 2`.

## What the three suites are for

| suite | tasks | built to measure |
|---|---:|---|
| `tasks_frontier` | 15 | documented language surface — a **regression** suite |
| `tasks_frontier_algo` | 14 | algorithmic depth, mostly reconstruction rather than cost |
| `tasks_frontier_spec` | 12 | **specification fidelity** — dense interacting rules |

Every task in all three carries a reference solution whose output was captured by
running it, so expected_stdout is provably achievable and byte-exact.

## Cross-tier results

| model | surface (15) | algorithmic (14) | spec (12) |
|---|---:|---:|---:|
| gemini-2.5-flash-lite | 14 *(5 retried)* | **7** *(8 retried)* | **6** *(7 retried)* |
| gemini-3.1-flash-lite | 14 *(2)* | 13 *(3)* | 12 *(2)* |
| gemini-3.5-flash | 15 *(1)* | 14 *(0)* | 12 *(0)* |
| DeepSeek-V4-Flash | 15 *(2)* | 14 *(0)* | 12 *(0)* |

`gemini-2.0-flash-lite` is retired and returns HTTP 404 from the OpenAI-compat
endpoint; all 12 of its "failures" measured nothing.

## Findings

**The surface suite saturates by design.** 14–15/15 everywhere. The guide is in
the prompt, so asking whether a model knows a documented rule tests recall, not
capability. Its value is catching compiler regressions — it already did, failing
on the chained-array-concat bug fixed in aether `53af57c`.

**Retry count discriminates where the score does not.** `gemini-3.1-flash-lite`
and `gemini-3.5-flash` both score 12/12 on spec, but one needed 2 repair rounds
and the other none; same on algorithmic (13 with 3 retries vs 14 with 0).
First-pass quality separates models that final scores rank identically. Report it
as a column.

**The frontier signal is probabilistic, not pass/fail.** DeepSeek-V4-Flash over
3 repeats (36 attempts) scored 36/36 exact but retried on 4:

| task | retry rate |
|---|---|
| `spec_league_table` | 2/3 |
| `spec_ledger_rounding` | 1/3 |
| `spec_semver_order` | 1/3 |
| other nine | 0/3 |

So it never fails, but stumbles on ~11% of attempts, concentrated in three tasks.
A single run cannot distinguish a 67%-retry task from a lucky clean one — an
earlier 3-task run showed 2 retries and the next showed 0. **Run the spec suite
with `--repeats 3` and report retry rate**, or the signal is invisible.

**Single-run deltas of one task are noise.** Re-running moved
`gemini-3.1-flash-lite` spec 11→12 and `gemini-2.5-flash-lite` spec 7→6. Only
multi-task gaps are real.

**`toon_fleet_rollup` is the most discriminating single task tested.** It failed
for both lite models and is DeepSeek's only reproducible stumble on the surface
suite. Nested TOON where the *intermediate* is missing (not the leaf) — worth
checking whether the guide's guard-the-intermediate advice lands.

## Gotchas that cost real time

**Raise `max_output_tokens`.** The existing gemini configs use 3000; these tasks
need 50–90 lines of Aether plus any thinking. Truncation scores as a model
failure. The low-tier runs used 16000.

**`--repeats` only works on direct destinations.** Through T'Ra the idempotency
key returns one cached job N times instead of N samples. Fixed harness-side in
pscal `b80a7d6b9`, but T'Ra's payload cannot yet exploit it.

**Check the model still exists.** `gemini-2.0-flash-lite` 404s; a dead model
produces a clean-looking 0/12.
