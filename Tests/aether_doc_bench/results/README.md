# Guided benchmark results

Fresh-era results land here, one directory per run set, dated.

Everything from before **2026-08-10** lives in [`history/`](history/HISTORY.md)
and is not comparable with what lands here: the guide roughly doubled in size
over that period, and the guide is this benchmark's independent variable. Read
`history/HISTORY.md` before quoting any older number.

Before running a sweep, read [`Docs/bench_runbook.md`](../../../Docs/bench_runbook.md).
It carries the pre-flight checklist, the bogus-score triage tree, and the
measurement rules that the pre-reset runs paid for.

Conventions worth keeping from the old era:

- **One directory per run set**, named `<subject>_<YYYYMMDD>/`, with a `README.md`
  giving the authoritative per-suite numbers and the aether + guide versions.
- **Keep failed artifacts** next to the rerun that replaced them, and say in the
  README which is authoritative. A rerun without its failure is uninterpretable.
- **A partial report's top-level `summary` lies** — it describes the run's
  configuration, not what it completed. Check `len(destinations[].variants)` and
  each `variants[].summary.total_cases`.

## Cross-era step changes in repair rounds

Repair-round numbers (Retried / Fixed / `resolved_after_repair`) are not
comparable across these harness changes, which landed together before B0:

- **Repair prompt source window (W1-07).** Until then a repair prompt showed
  only the first 1,200 characters of the previous program (the same cap as
  stdout and stderr), which cut 22-47% of the lines of large and hard tasks,
  often including the line the diagnostic cites. It now shows the whole
  program up to `--repair-source-limit` (8,000 characters), and above that the
  head, +/-20 lines round the cited line and the tail. Runs of identical
  stderr lines are collapsed first. Each repair attempt records
  `source_truncated`, `source_cap` and `stderr_collapsed`. Expect repair rescue
  on large/hard suites to step up; do not read that step as a model or guide
  effect.
- **Timeouts (W1-06).** A program killed at its time limit is now a measured
  attempt (rc 124, `timeout:<task>`) that gets a repair round, where it used
  to be filed as a generation error with no attempts.
- **Temp paths (W1-06).** stderr in repair prompts names the program as
  `task.aether:N:` instead of an absolute temp path.
- **Empty first attempts (2026-10-09 and 2026-10-12).** Two steps. From harness
  20426ef7 a first attempt that holds no program because the model used up its
  token or time budget is a measured model failure (`no_answer`) and not an
  infrastructure failure to re-run. From 6a44b26d (the owner's rule of
  2026-10-12) such a first attempt also ends the case: it gets no repair round
  (`--repair-after-no-answer` gives the earlier behaviour, and `run_config`
  records which applied). First-attempt numbers are unaffected. After-repair
  numbers are, for models that reason to the cap: summaries carry both the
  number as run (`FX`) and the number the rule gives (`FX nr`), and a board run
  after the rule has only the second.
  Re-scored on 2026-10-12, every report tracked here before the rule (the
  history boards, local_tiers_20260811, both Apple on-device runs, the local B0
  boards and the devstral set: 3,197 cases) holds no case in which an empty
  first attempt was later repaired into a pass, so none of their published
  numbers change. The same holds for the eight archived runs behind the reports
  in `Docs/benchmarks/` (cs-aug19 and cs-aug20 boards, the arity-fix and
  early-stopping runs: 5,487 cases, 21 empty first attempts, none repaired). The hosted twin of B0 is where the rule bites (its
  qwen3.5-9b after-repair rate on the small guide is 90.3% as run and 71.3%
  under the rule); it is published with both columns.

## Replay and the instrument log

`tools/replay_bench.py --aether-bin BIN [GLOB ...]` re-grades every stored
program (default: the tracked `results/**`, minus `rejected/`) on another
binary and reports fail->pass, pass->fail and failure-class transitions. A
pass->fail, or a move into silent_wrong, must be waived in
`../replay_waivers.json` (keyed by the CHANGELOG version that declared the
break, plus task id and source sha256) or the replay exits 1. Untracked sets
such as `harness_out/**` and the cs-aug20 `none` evals are passed as extra
globs on the machine that holds them.

`tools/instrument_check.sh --aether-bin BIN --log` runs the oracle check, the
replay, the corpus recapture `--check` and (once they exist) the corpus build
gates and the guide contamination check, and appends one row to
`instrument_log.csv`. Baseline row, 2026-10-07, on the standalone 2026-10-06-1
binary: 274 tracked first attempts 242 -> 245 (11 fail->pass, 8 pass->fail),
every regression waived (12 programs: 11 under 2026-07-26-1 0-based Text, 1
under 2026-08-09-1 array `+`), 0 unwaived, 140/140 references.
