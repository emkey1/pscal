#!/usr/bin/env python3
"""VM performance baseline runner (VM 2.0 plan, Phase 0 item 2).

Runs each benchmark program in Tests/vm_bench/ N times through the Pascal
frontend with --no-cache, takes the median of the benchmark's self-reported
execution time (RealTimeClock delta, which excludes compile time), verifies
the deterministic check value, and appends one JSON line per invocation to
history.jsonl with git SHAs and the date so per-phase history accumulates.

Usage:
  python3 Tests/vm_bench/run_vm_bench.py                 # all benches, 5 runs
  python3 Tests/vm_bench/run_vm_bench.py --runs 9
  python3 Tests/vm_bench/run_vm_bench.py --only arith,json
  python3 Tests/vm_bench/run_vm_bench.py --label "phase2b-slot-globals"
  python3 Tests/vm_bench/run_vm_bench.py --no-record     # don't touch history
"""

import argparse
import json
import os
import platform
import re
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
HISTORY = BENCH_DIR / "history.jsonl"
RUN_TIMEOUT_S = 600  # generous; a hung VM should not hang the runner forever

# name -> (source file, expected deterministic check value)
BENCHES = {
    "arith":   ("arith.p",   "820389702"),
    "calls":   ("calls.p",   "19641820"),
    "strings": ("strings.p", "9766500"),
    "globals": ("globals.p", "3547984"),
    "json":    ("json.p",    "8011500"),
    "io_http": ("io_http.p", "2205120"),
}

CHECK_RE = re.compile(r"^check=(\S+)$", re.M)
ELAPSED_RE = re.compile(r"^elapsed_s=([0-9.]+)$", re.M)


def git_sha(repo: Path) -> str:
    try:
        sha = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=30,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", str(repo), "status", "--porcelain",
             "--untracked-files=no"],
            capture_output=True, text=True, timeout=30,
        ).stdout.strip()
        return sha + ("-dirty" if dirty else "") if sha else "unknown"
    except Exception:
        return "unknown"


def run_once(bin_path: Path, src: Path, scratch: Path):
    """One benchmark run. Returns (self_timed_s, wall_s, check, stderr)."""
    env = dict(os.environ, VM_BENCH_TMP=str(scratch))
    t0 = time.monotonic()
    proc = subprocess.run(
        [str(bin_path), "--no-cache", str(src)],
        cwd=str(scratch), env=env,
        capture_output=True, text=True, timeout=RUN_TIMEOUT_S,
    )
    wall = time.monotonic() - t0
    if proc.returncode != 0:
        raise RuntimeError(
            f"{src.name} exited {proc.returncode}\n"
            f"stdout: {proc.stdout[-2000:]}\nstderr: {proc.stderr[-2000:]}")
    m_check = CHECK_RE.search(proc.stdout)
    m_el = ELAPSED_RE.search(proc.stdout)
    if not m_check or not m_el:
        raise RuntimeError(
            f"{src.name}: missing check=/elapsed_s= in output\n"
            f"stdout: {proc.stdout[-2000:]}\nstderr: {proc.stderr[-2000:]}")
    return float(m_el.group(1)), wall, m_check.group(1), proc.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", type=Path, default=DEFAULT_BIN,
                    help=f"pascal frontend binary (default {DEFAULT_BIN})")
    ap.add_argument("--runs", type=int, default=5,
                    help="runs per benchmark (default 5)")
    ap.add_argument("--only", default="",
                    help="comma-separated subset of benchmark names")
    ap.add_argument("--label", default="",
                    help="free-text tag recorded with the results "
                         "(e.g. the VM 2.0 phase being measured)")
    ap.add_argument("--no-record", action="store_true",
                    help="skip appending to history.jsonl")
    args = ap.parse_args()

    bin_path = args.bin.resolve()
    if not bin_path.is_file():
        sys.exit(f"error: pascal binary not found at {bin_path} "
                 f"(build it or pass --bin)")

    names = list(BENCHES)
    if args.only:
        names = [n.strip() for n in args.only.split(",") if n.strip()]
        unknown = [n for n in names if n not in BENCHES]
        if unknown:
            sys.exit(f"error: unknown benchmark(s) {unknown}; "
                     f"available: {list(BENCHES)}")

    results = {}
    failures = []
    with tempfile.TemporaryDirectory(prefix="vm_bench_") as scratch_s:
        scratch = Path(scratch_s)
        for name in names:
            src_name, expected = BENCHES[name]
            src = BENCH_DIR / src_name
            elapsed, walls = [], []
            check_ok = True
            print(f"[{name}] ", end="", flush=True)
            for i in range(args.runs):
                try:
                    el, wall, check, _ = run_once(bin_path, src, scratch)
                except Exception as e:
                    failures.append(f"{name} run {i + 1}: {e}")
                    print("E", end="", flush=True)
                    continue
                if check != expected:
                    check_ok = False
                    failures.append(
                        f"{name} run {i + 1}: check mismatch "
                        f"(got {check}, expected {expected}) — "
                        f"semantic change, result NOT comparable")
                elapsed.append(el)
                walls.append(wall)
                print(".", end="", flush=True)
            if elapsed:
                results[name] = {
                    "median_s": round(statistics.median(elapsed), 6),
                    "min_s": round(min(elapsed), 6),
                    "max_s": round(max(elapsed), 6),
                    "wall_median_s": round(statistics.median(walls), 6),
                    "runs": len(elapsed),
                    "check_ok": check_ok,
                }
                print(f" median {results[name]['median_s']:.3f}s "
                      f"(min {results[name]['min_s']:.3f} "
                      f"max {results[name]['max_s']:.3f}, "
                      f"wall {results[name]['wall_median_s']:.3f}s)")
            else:
                print(" all runs failed")

    record = {
        "date_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "label": args.label,
        "pbuild_sha": git_sha(REPO_ROOT),
        "pscal_core_sha": git_sha(REPO_ROOT / "components" / "pscal-core"),
        "pascal_sha": git_sha(REPO_ROOT / "components" / "pascal"),
        "host": platform.node().split(".")[0],
        "machine": platform.machine(),
        "bin": str(bin_path),
        "runs_per_bench": args.runs,
        "results": results,
    }

    print()
    print(f"{'benchmark':<10} {'median_s':>9} {'min_s':>9} {'max_s':>9}")
    for name, r in results.items():
        flag = "" if r["check_ok"] else "  CHECK-MISMATCH"
        print(f"{name:<10} {r['median_s']:>9.3f} {r['min_s']:>9.3f} "
              f"{r['max_s']:>9.3f}{flag}")

    if failures:
        print("\nFAILURES:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)

    if not args.no_record and results:
        with open(HISTORY, "a") as fh:
            fh.write(json.dumps(record, sort_keys=True) + "\n")
        print(f"\nrecorded -> {HISTORY}")
    elif args.no_record:
        print("\n(not recorded: --no-record)")

    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
