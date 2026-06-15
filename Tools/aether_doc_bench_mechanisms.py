#!/usr/bin/env python3
"""Per-mechanism pass-rate breakdown for an aether_doc_bench report.

Surfaces which Aether mechanisms a model fails, instead of hiding them in a
single headline number. Repeat-aware: when a report has --repeats > 1, each
task's verdict is the MAJORITY across its repeats (noise-robust), and tasks that
pass some-but-not-all repeats are flagged as FLAKY (the temp-0 noise band).

  python3 Tools/aether_doc_bench_mechanisms.py <report.json> [--doc none] [--tasks tasks.json]
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_TASKS = REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks.json"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("report", type=pathlib.Path, help="benchmark report JSON")
    ap.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    ap.add_argument("--doc", default="", help="restrict to one doc variant (full/small/none)")
    args = ap.parse_args()

    tasks = {t["id"]: t for t in json.loads(args.tasks.read_text())["tasks"]}
    report = json.loads(args.report.read_text())

    for dest in report.get("destinations", []):
        for variant in dest.get("variants", []):
            if args.doc and variant.get("doc_name") != args.doc:
                continue
            # Group repeats per task -> noise-robust majority verdict.
            by_task: dict[str, list[bool]] = collections.defaultdict(list)
            for r in variant.get("results", []):
                by_task[r["task_id"]].append(bool((r.get("run") or {}).get("exact_stdout_match")))
            verdict: dict[str, bool] = {}
            flaky: list[tuple[str, int, int]] = []
            for tid, runs in by_task.items():
                pc, n = sum(runs), len(runs)
                verdict[tid] = pc * 2 > n  # passes a strict majority of repeats
                if 0 < pc < n:
                    flaky.append((tid, pc, n))
            reps = max((len(v) for v in by_task.values()), default=1)

            mech_pass: collections.Counter = collections.Counter()
            mech_total: collections.Counter = collections.Counter()
            for tid, ok in verdict.items():
                for mech in tasks.get(tid, {}).get("mechanisms", ["untagged"]):
                    mech_total[mech] += 1
                    mech_pass[mech] += 1 if ok else 0
            total = len(verdict)
            won = sum(verdict.values())
            tag = f"  (majority of {reps} repeats)" if reps > 1 else ""
            print(f"== {dest.get('destination_id')} [{variant.get('doc_name')}]  "
                  f"overall {won}/{total} ({won/total:.2f}){tag} ==")
            for mech in sorted(mech_total):
                p, t = mech_pass[mech], mech_total[mech]
                bar = "" if p == t else "  <-- gap" if p == 0 else "  <-- partial"
                print(f"   {mech:24s} {p}/{t}{bar}")
            fails = sorted(t for t, ok in verdict.items() if not ok)
            if fails:
                print("   failing tasks:", ", ".join(fails))
            if flaky:
                print("   FLAKY (noise band):", ", ".join(f"{t}={p}/{n}" for t, p, n in sorted(flaky)))
            print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
