#!/usr/bin/env python3
"""Phase 1e verifier load-time overhead check (Docs/pscal_vm2_plan.md §5.5,
§8): the verifier runs once per chunk load, so its cost must be negligible
next to everything else a cache-hit run already does (process startup,
cache lookup/hash, section deserialization, program execution).

Measures whole-process wall-clock time for a cache-hit run of a
representative program (calls.p, which has real procedures so the
per-procedure stack-depth pass has non-trivial work to do) with the
verifier on vs. off (PSCAL_VM_SKIP_VERIFY=1), and reports the median
difference. This is deliberately an external wall-clock comparison (not the
benchmarks' internal RealTimeClock self-timing, which starts only after the
chunk is already loaded and so never sees this cost at all).

Usage:
  python3 Tests/vm_bench/bench_verify_overhead.py
  python3 Tests/vm_bench/bench_verify_overhead.py --runs 41 --bin build/bin/pascal
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parent.parent
DEFAULT_BIN = REPO_ROOT / "build" / "bin" / "pascal"
HISTORY = BENCH_DIR / "verify_overhead_history.jsonl"


def git_sha(repo: Path) -> str:
    try:
        sha = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, timeout=30).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no"],
                                capture_output=True, text=True, timeout=30).stdout.strip()
        return sha + ("-dirty" if dirty else "") if sha else "unknown"
    except Exception:
        return "unknown"


def time_runs(binary, prog, runs, cache_home, skip_verify):
    env = dict(os.environ)
    env["HOME"] = cache_home
    if skip_verify:
        env["PSCAL_VM_SKIP_VERIFY"] = "1"
    else:
        env.pop("PSCAL_VM_SKIP_VERIFY", None)
    times = []
    for _ in range(runs):
        t0 = time.perf_counter()
        proc = subprocess.run([str(binary), str(prog)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
        times.append(time.perf_counter() - t0)
        if proc.returncode != 0:
            raise RuntimeError(f"{prog} exited {proc.returncode} (skip_verify={skip_verify})")
    return times


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=str(DEFAULT_BIN))
    ap.add_argument("--prog", default=str(BENCH_DIR / "calls.p"))
    ap.add_argument("--runs", type=int, default=31)
    ap.add_argument("--label", default="")
    ap.add_argument("--no-record", action="store_true")
    args = ap.parse_args()

    binary = Path(args.bin)
    if not binary.exists():
        print(f"binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as cache_home:
        # Prime the cache once (cold compile), untimed.
        subprocess.run([str(binary), args.prog], env={**os.environ, "HOME": cache_home},
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60, check=True)

        verify_on = time_runs(binary, args.prog, args.runs, cache_home, skip_verify=False)
        verify_off = time_runs(binary, args.prog, args.runs, cache_home, skip_verify=True)

    med_on = statistics.median(verify_on)
    med_off = statistics.median(verify_off)
    delta = med_on - med_off

    print(f"verify ON  median: {med_on*1000:.3f} ms (min {min(verify_on)*1000:.3f}, max {max(verify_on)*1000:.3f})")
    print(f"verify OFF median: {med_off*1000:.3f} ms (min {min(verify_off)*1000:.3f}, max {max(verify_off)*1000:.3f})")
    print(f"delta (verify cost, median-vs-median): {delta*1000:+.3f} ms "
          f"({'noise-dominated (verify cost not distinguishable from run-to-run jitter)' if abs(delta) < (statistics.stdev(verify_on) + statistics.stdev(verify_off)) else 'measurable'})")

    if not args.no_record:
        entry = {
            "date_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "host": os.uname().nodename.split(".")[0],
            "machine": os.uname().machine,
            "label": args.label,
            "pbuild_sha": git_sha(REPO_ROOT),
            "pscal_core_sha": git_sha(REPO_ROOT / "components" / "pscal-core"),
            "pascal_sha": git_sha(REPO_ROOT / "components" / "pascal"),
            "bin": str(binary),
            "prog": os.path.basename(args.prog),
            "runs": args.runs,
            "verify_on_median_s": med_on,
            "verify_on_min_s": min(verify_on),
            "verify_on_max_s": max(verify_on),
            "verify_off_median_s": med_off,
            "verify_off_min_s": min(verify_off),
            "verify_off_max_s": max(verify_off),
            "delta_s": delta,
        }
        with open(HISTORY, "a") as f:
            f.write(json.dumps(entry) + "\n")
        print(f"appended to {HISTORY}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
