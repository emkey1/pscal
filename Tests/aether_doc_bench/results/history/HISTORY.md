# Guided benchmark — historical results (pre-1.0)

Everything in this directory predates the fresh-start line drawn on
**2026-08-10**, when the guide and the compiler were judged to be approaching
1.0 quality. It is kept for provenance, not for comparison.

## Read this before quoting any number below

**These eras are not comparable with each other.** The guided benchmark measures
how well a model turns *the guide* into working Aether, so the guide is the
independent variable — and it more than doubled in size across the period, while
the compiler moved under it too:

| era | aether | guide sizes |
|---|---|---|
| 2026-06-30 | `2026-06-26-1` | full 41,067 b · small 22,073 b |
| 2026-07-04/05 | `2026-07-04-2` | full 50,298 b · small 27,639 b |
| 2026-07-30 | `2026-07-26-9` | full 82,056 b · **medium** 48,297 b |
| 2026-08-10 | `2026-08-09-1` | medium |

A rising score across eras may mean a better guide, a better compiler, an easier
task manifest, or a different model lineup. Usually several at once. Within a
single era the comparisons are sound, which is what makes these runs worth
keeping.

The task manifests also grew and changed (`tasks_v2_pos` 30 → 35 cases,
`tasks_hard` 8 → 14, plus `nontoon` and the three `frontier` suites), so even
same-named suites differ in size between eras.

## Eras

### 2026-06-30 — Ornith-1.0-35B, first run · `ornith/`

First guided run of Ornith-1.0-35B (NVFP4 on claw1, via T'Ra). aether
`2026-06-26-1`. Landed by `c3c1a628f` with the runner.

| suite | full | small |
|---|---:|---:|
| simple (30) | 29/30 | 30/30 |
| large (8) | 8/8 | 8/8 |
| cs (19) | 15/19 | 18/19 |

### 2026-07-04 — gemini-2.5-flash guide sweep · `full/`

aether `2026-07-04-2`, the 50 kb full guide vs the 27 kb small guide, one model.
**full 52/57 (91.2%) · small 51/57 (89.5%).** Never committed; kept here as the
only single-model full-vs-small datapoint from that guide generation.

### 2026-07-05 — Ornith-1.0-35B, second run · `ornith_20260705/`

Same model and destination as June 30, five days and one aether/guide bump later.
Landed by `10f899c67`; see that directory's own `README.md` for authoritative
numbers, the two harness failures, and why cs is stored as two repeats.

| suite | full | small |
|---|---:|---:|
| simple (30) | 30/30 | 30/30 |
| large (8) | 8/8 *(rerun)* | 7/8 |
| cs (19) | 17/19, 18/19 | 18/19, 17/19 |

### 2026-07-30 — guide full vs medium, 17 models · `guide_full_vs_medium_*_20260729/`

The sweep that justified the medium guide. aether `2026-07-26-9`, full (82 kb) vs
medium (48 kb), four suites (simple 35, large 14, cs 19, nontoon 5).

| vendor | models | full | medium |
|---|---:|---:|---:|
| Gemini | 7 | 489/511 (95.7%) | 484/511 (94.7%) |
| OpenAI | 10 | 727/730 (99.6%) | 727/730 (99.6%) |

**Halving the guide cost ~1 point on Gemini and nothing measurable on OpenAI.**
That is the finding that carried forward: the medium guide became the default,
and every run after this one uses it. Driver logs in `logs/`; the per-model
JSONs are the two directories (136 files, ~24 MB, untracked).

`out_guided_cloud/` holds an earlier partial cloud run of the same shape.

### 2026-08-10 — frontier suites · `frontier_suites_20260810.md`

The last pre-reset run, and the most methodologically careful: three new suites
(surface 15, algorithmic 14, spec 12) against four models on aether
`2026-08-09-1`, every task carrying a reference solution whose output was
captured by running it.

Read the document itself rather than a summary here. It is the only one of these
that retracts one of its own findings — `toon_fleet_rollup` turned out to be
measuring schema guessing, because `build_prompt` never puts `task.files` in the
prompt — and the retraction is more instructive than the scores.

Its durable, version-independent lessons have been hoisted into
[`Docs/bench_runbook.md`](../../../../Docs/bench_runbook.md) so they survive this
reset. The scores stay here.

## Layout

| path | tracked | what |
|---|---|---|
| `ornith/` | ✅ 3 JSONs | June 30 run + untracked hang diagnostics from July 5 |
| `ornith_20260705/` | ✅ 6 files | July 5 run set, with its own README |
| `frontier_suites_20260810.md` | ✅ | Aug 10 findings |
| `full/` | ❌ | July 4 gemini sweep |
| `guide_full_vs_medium_20260729/` | ❌ 56 files | July 30, Gemini |
| `guide_full_vs_medium_openai_20260729/` | ❌ 80 files | July 30, OpenAI |
| `out_guided_cloud/` | ❌ | earlier partial cloud run |
| `ornith_archive_20260630/` | ❌ | byte-identical copy of `ornith/` as committed |
| `ornith_archive_20260705/` | ❌ | July 5 working-tree copies, pre-assembly |
| `logs/` | ❌ | driver logs for the above |

The untracked ~33 MB is deliberately untracked — raw per-case JSON with full
model outputs, too bulky to commit and reconstructible in spirit from the
summaries above. It is also **not backed up anywhere else**, so do not clean it
out casually.

## Fresh era

New results go in `Tests/aether_doc_bench/results/`, not here. Nothing in this
directory should be extended; if a historical run needs a rerun, it is a new run
in the live area against the current guide.
