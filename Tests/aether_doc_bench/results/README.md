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
