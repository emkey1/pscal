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

**~~`toon_fleet_rollup` is the most discriminating single task tested.~~
RETRACTED 2026-08-10 — the task was mis-authored and was measuring schema
guessing.** Its prompt stated where `mem` lived (`spec.mem.gb`) but never where
`cores` lived, and `build_prompt` does not put `task.files` in the prompt, so no
model ever saw `fleet.json`. Singling out `mem` as "NESTED" further implied
`cores` was top-level. **All 8 models across 3 vendors read `cores` off the entry
node**, and 4 of them — gemini-3.5-flash, gpt-5-mini, gpt-5.4-nano and
DeepSeek-V4-Flash — emitted byte-identical wrong output (`cores=0` on every row,
`mem` correct). Convergence that tight is a prompt defect, not a capability
signal.

Note what this means for the guide: the nested-`mem` handling the task actually
set out to test was **correct in every model that compiled**. The guard-the-
intermediate advice landed. Fixed in manifest `2026-08-10-2` by inlining
`fleet.json` into the prompt and naming `spec.cores` explicitly; expected_stdout
and the other 14 tasks are unchanged, so earlier scores stay comparable.

The task did still catch real errors as authored: three models put
`toon_parse_file` outside an `fx` block (FX-001) and one produced SYN-001.

`spec_league_table` was audited for the same defect and is **clean** — it names
every field and its nesting, and two models solved it first try — so the spec
suite's retry-rate findings below stand.

### Verification: both fixes measured separately

`components/aether` was still pinned to guide `2026-08-08-1`, which allowed the
task fix and the guide fix to be separated instead of confounded. Three models
that had all produced the identical `cores=0` output, 3 repeats each:

| condition | attempt-0 `cores` right | used `toon_key_or` | attempt-0 exact | repairs |
|---|---:|---:|---:|---:|
| original task + old guide *(8 models, 1 each)* | **0/8** | 0/8 | 0/8 | 8 |
| fixed task + old guide *(3 models × 3)* | **9/9** | 1/9 | 5/9 | 2 |
| fixed task + new guide *(3 models × 3)* | **9/9** | **9/9** | **8/9** | **0** |

The task fix alone eliminates the schema error outright (0/8 → 9/9). The guide
fix then takes first-attempt exactness from 5/9 to 8/9 and repairs to zero, with
every run adopting `toon_key_or` — a documentation change worth ~3 tasks in 9 on
this one task, which is the size of gap that separated whole model tiers earlier
in this table.

The one surviving failure in each arm is the same gpt-5-mini habit: a trailing
`println("")` adding a blank line (FMT-001). All values correct. Unrelated to
TOON.

## Gotchas that cost real time

**Raise `max_output_tokens`.** The existing gemini configs use 3000; these tasks
need 50–90 lines of Aether plus any thinking. Truncation scores as a model
failure. The low-tier runs used 16000.

**`--repeats` only works on direct destinations.** Through T'Ra the idempotency
key returns one cached job N times instead of N samples. Fixed harness-side in
pscal `b80a7d6b9`, but T'Ra's payload cannot yet exploit it.

**Check the model still exists.** `gemini-2.0-flash-lite` 404s; a dead model
produces a clean-looking 0/12.

**`task.files` is never shown to the model.** `build_prompt` materializes files
on disk for the *run*, but does not put their contents in the prompt. Any task
providing a file must therefore state its schema in prose, or the model is
guessing — and a guess that fails looks exactly like a capability failure. 37 of
191 tasks provide files; `toon_fleet_rollup` is the one confirmed to have been
under-specified. When a task provides a file, either inline it (the `tasks_hard`
convention) or name every field and its nesting.

**Identical wrong output across vendors means the prompt, not the models.** The
cheapest tell in the whole harness. If two independently-trained models emit the
same wrong bytes, stop investigating the models.
