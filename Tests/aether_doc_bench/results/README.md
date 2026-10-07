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
