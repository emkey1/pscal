# Ornith-1.0-35B guided benchmark — 2026-07-05 run set

Second Ornith run session, five days after the committed `ornith/` baseline
(2026-06-30). Both the compiler and the guides moved between the two, so this is
a **separate run set, not a replacement** for the June 30 numbers.

| | 2026-06-30 (`ornith/`) | 2026-07-05 (this dir) |
|---|---|---|
| aether | `2026-06-26-1` | `2026-07-04-2` |
| guide `full` | 41,067 b | 50,298 b |
| guide `small` | 22,073 b | 27,639 b |

Destination `claw1-ornith-1.0-35b-nvfp4` via T'Ra, `destinations.tra.json`,
identical task manifests (`tasks_v2_pos` 2026-06-21-1, `tasks_hard` 2026-06-21-1,
`tasks_cs` 2026-06-23-1). Guide size is this benchmark's independent variable, so
scores here are **not** comparable case-for-case with June 30.

## Authoritative numbers

| suite | variant | exact / total | file |
|---|---|---:|---|
| simple (`tasks_v2_pos`) | full | 30/30 | `ornith_simple.json` |
| simple | small | 30/30 | `ornith_simple.json` |
| large (`tasks_hard`) | full | 8/8 | `ornith_large_full_rerun.json` |
| large | small | 7/8 | `ornith_large.json` |
| cs (`tasks_cs`) | full | 17/19, 18/19 | `ornith_cs_repeat1/2.json` |
| cs | small | 18/19, 17/19 | `ornith_cs_repeat1/2.json` |

## Why the file layout is not one-file-per-suite

The session hit two harness failures. Both are recorded here rather than papered
over, because the failed artifacts are what make the reruns interpretable.

**`ornith_large.json` — take `small` only.** Its `full` variant scored 1/8 with
`generated_ok=1`, a harness/generation failure rather than a model result. It was
re-run as `ornith_large_full_rerun.json`, which scored 8/8. The `small` variant in
the original file (7/8) completed normally and exists in no other file, so the
original is kept and the `full` half of it should be ignored.

**cs is two repeats, and the first attempt is absent.** The initial cs run hung
on `cs_merge_sort` and produced a truncated artifact: 14 of 19 tasks, `full`
variant only, no `small` variant at all, while its own `summary` block still
claimed `total_cases_per_destination: 19, doc_variants: 2`. That file is **not**
committed; it survives untracked as
`../ornith/ornith_cs.json.partial_hang_backup`, alongside the single-task probes
used to isolate the hang
(`cs_merge_sort_smoketest`, `cs_quick_sort_diffcheck`, `cs_bfs_diffcheck`). The
two complete reruns are committed here as `repeat1` (16:45Z) and `repeat2`
(17:43Z).

Keep both cs repeats. They disagree by one task in each direction (full 17→18,
small 18→17), which is a useful in-tree example of the noise floor: single-run,
single-task deltas in this harness are not signal.

## Caveat on a truncated `summary`

A partial report's top-level `summary` reflects what the run was *configured* to
do, not what it completed. Check `len(destinations[].variants)` and each
`variants[].summary.total_cases` before trusting any rollup here or elsewhere.
