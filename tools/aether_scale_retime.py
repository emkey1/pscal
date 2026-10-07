#!/usr/bin/env python3
"""Re-time the scale suite's references on THIS host (W1-23).

A scale task's timeout is `max(20, ceil(10 x its reference's time on the host
that runs the board))`, recorded in the task with the measurement
(scale.timing). The times shipped in tasks_scale.json were taken on a busy
development machine; before board S0, run this on the board host against the
board's binary:

  python3 tools/aether_scale_retime.py --aether-bin <bin>            # report only
  python3 tools/aether_scale_retime.py --aether-bin <bin> --write    # rewrite timeouts + timing

Each reference runs --repeats times (the median counts) through the harness's
own compile_and_run, so the sandbox, cwd and generated inputs are what a
scored case gets. A reference that fails exits 1 and nothing is written.
--write also re-times the python references and, when any timeout changes,
bumps the manifest version: timeout_seconds is a grading field, so a paired
replay must see the change.
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import pathlib
import platform
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_doc_bench as adb  # noqa: E402

SCALE = adb.REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks_scale.json"
PY_REFS = adb.REPO_ROOT / "Tests" / "aether_doc_bench" / "py_refs"


def timeout_for(seconds: float) -> int:
    return max(20, math.ceil(10 * seconds))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aether-bin", type=pathlib.Path, default=adb.DEFAULT_AETHER_BIN)
    ap.add_argument("--tasks", type=pathlib.Path, default=SCALE)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)

    raw = json.loads(args.tasks.read_text(encoding="utf-8"))
    tasks = {t.task_id: t for t in adb.load_tasks(args.tasks)}
    version, _ = adb.capture_aether_version(args.aether_bin)
    run_args = argparse.Namespace(aether_bin=str(args.aether_bin), sandbox_deny="net,proc", aether_args=[])
    changed = failed = 0
    for item in raw["tasks"]:
        task = tasks[item["id"]]
        task.timeout_seconds = 3600
        times = []
        for _ in range(max(1, args.repeats)):
            run = adb.compile_and_run(task, task.reference_solution or "", run_args)
            if not run["exact_stdout_match"]:
                failed += 1
                print(f"[FAIL] {item['id']}: reference does not match on aether {version} (rc={run['returncode']})")
                break
            times.append(run["elapsed_seconds"])
        if len(times) != max(1, args.repeats):
            continue
        ref = round(statistics.median(times), 2)
        new_timeout = timeout_for(ref)
        old_timeout = int(item.get("timeout_seconds", 20))
        print(f"{item['id']}: reference {ref:.2f}s -> timeout {new_timeout}s (was {old_timeout}s)")
        if args.write:
            timing = item.setdefault("scale", {}).setdefault("timing", {})
            timing["reference_seconds"] = {version: ref}
            py = PY_REFS / f"{item['id']}.py"
            if py.is_file():
                timing["python_seconds"] = round(adb.run_python_task(task, py.read_text())["elapsed_seconds"], 2)
            timing["platform"] = f"{platform.system().lower()}-{platform.machine()}"
            timing["measured"] = datetime.date.today().isoformat()
            if new_timeout != old_timeout:
                item["timeout_seconds"] = new_timeout
                changed += 1
    if failed:
        return 1
    if args.write:
        if changed:
            stamp = datetime.date.today().isoformat()
            n = 1
            while f"{stamp}-{n}" <= str(raw.get("version", "")) and str(raw.get("version", "")).startswith(stamp):
                n += 1
            raw["version"] = f"{stamp}-{n}"
        args.tasks.write_text(json.dumps(raw, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"wrote {adb.display_path(args.tasks)}: {changed} timeout(s) changed, version {raw['version']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
