# Benchmark run reports

One document per completed benchmark run. Each is about **its own run** — the
settings it used, the scores it produced, and anything that made those scores
mean something other than what they appear to. Earlier boards are referenced
only where that explains a change to the language, the harness or the suite.

For the cross-run narrative — which corpus change moved what, and why — see
[`aether_specialization_findings.md`](../../components/aether/docs/aether_specialization_findings.md)
in the aether repo. These files are the per-run record underneath it.

## Runs

| report | corpus | models | repair | what it was for |
|---|---|---|---|---|
| [cs-aug19](cs-aug19.md) | cs-aug19 | 6 | 0 | baseline before the 0-based `Text` work |
| [cs-aug20-8b](cs-aug20-8b.md) | cs-aug20 | 3 | 0 | first board on the corrected corpus |
| [cs-aug20-9b](cs-aug20-9b.md) | cs-aug20 | 3 | 0 | 9B family, after the serving fix |
| [cs-aug20-large](cs-aug20-large.md) | cs-aug20 | 5 | 2 | 14B–35B tier, first time on this corpus |
| [cs-aug20-small-repair2](cs-aug20-small-repair2.md) | cs-aug20 | 6 | 2 | 8B/9B re-scored so both tiers match |
| [early-stopping-experiment](early-stopping-experiment.md) | cs-aug20 | 1 | 2 | is early stopping ending runs too soon? |
| [diagnostics-fix](diagnostics-fix.md) | cs-aug20 | 5 | 2 | did fixing four compiler diagnostics move scores? |

## Reading these

**Scores are per-task, majority of repeats.** A suite total that is not divisible
by the repeat count means the repeats disagreed — generation is ~97% but not
100% deterministic at temperature 0, because vLLM's continuous batching perturbs
numerics by batch composition.

**`first -> repair` cells** show the first attempt and the result after repair
attempts. First-attempt numbers are the ones comparable to repair-0 boards; the
harness defaults `--repair-attempts` to 0, while the older cs-aug4 board used 1.

**Check the Integrity section before trusting a cell.** `generated_ok` below the
case count means empty or failed generations, usually a reasoning model returning
no content, and that cell is a floor rather than a score.

**The grader is pinned per board.** Comparing two boards graded by different
compilers reads as a model result when it is a tooling change. Where the compiler
*is* the variable, the report says so.

## Regenerating

`scratchpad/gen_bench_docs.py` derives everything factual — scores, suite sizes,
repeat counts, `generated_ok`, grader and task-suite versions — from the result
JSON under `/storage/aether_eval_*` on the claws. Per-run commentary is supplied
in the script's `RUNS` table. Re-run it to refresh a partial report after a run
finishes.
