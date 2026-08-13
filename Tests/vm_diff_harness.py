#!/usr/bin/env python3
"""Differential VM harness (VM 2.0 plan, Phase 0 item 1).

Runs a program corpus through TWO pscal VM builds and byte-compares
stdout, stderr, and exit codes, reporting any divergence.  Every VM 2.0
phase gates on a zero-diff run of this harness (Docs/pscal_vm2_plan.md §4).

Corpus:
  pascal   components/pascal/tests/Pascal/<Name>       (extensionless fixtures)
  clike    components/clike/tests/*.cl
  rea      components/rea/tests/rea/*.rea               (honours .args files)
  aether   components/aether/tests/*.aether
  docbench sampled generated programs out of Tests/aether_doc_bench results
           JSONs (opt-in via --docbench-sample N)

Each program is run with the matching frontend binary from --vm-a and
--vm-b (a build/bin directory or a build tree containing bin/), always
with --no-cache, with the same cwd/stdin/args conventions as the real
suites.  A unit whose A/B outputs differ is re-run once per side; if
either side disagrees with itself the unit is classified NONDET instead
of DIFF, so inherently nondeterministic programs (threads, clocks,
network) never produce false gate failures.

Statuses:
  MATCH          identical exit/stdout/stderr
  MATCH_TIMEOUT  both builds exceeded --timeout (not a divergence; reported)
  DIFF           reproducible divergence -> artifacts under <out>/diffs/,
                 nonzero exit
  NONDET         a side disagreed with itself on re-run (reported, not fatal)
  ERROR          harness-level failure (missing binary, ...), nonzero exit

Resumable: one JSON per unit under <out>/results/; finished units are
skipped on re-invocation (see --rerun).  The summary and the gate exit
code are recomputed every run from all recorded results for the selected
units, so a resumed run still fails on previously recorded diffs.

Typical use:
  # self-check (same build on both sides; inventories NONDET units)
  Tests/vm_diff_harness.py --vm-a build/bin --vm-b build/bin

  # real gate between two builds
  Tests/vm_diff_harness.py --vm-a build/bin --vm-b build-vm2/bin \
      --out Tests/vm_diff_out/phase1

  # smoke-test a single unit first (repo convention for new drivers)
  Tests/vm_diff_harness.py --vm-a build/bin --vm-b build/bin \
      --only pascal/BoolTest
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent
FRONTENDS = ("pascal", "clike", "rea", "aether")
DEFAULT_SUITES = ["pascal", "clike", "rea", "aether"]
ALL_SUITES = DEFAULT_SUITES + ["docbench"]

# Interactive SDL fixtures the real suites also special-case/skip; they
# block on keyboard input and would burn a full timeout per side.
BUILTIN_SKIP = {
    "pascal/SDLFeaturesTest": "interactive SDL fixture (needs keypress)",
    "pascal/GetScreenSizeTest": "interactive SDL fixture",
}

ARTIFACT_CAP = 8 * 1024 * 1024  # bytes written per diff artifact file


@dataclass
class Unit:
    suite: str
    name: str            # unique within suite
    frontend: str        # which binary to use
    cwd: Path
    argv_tail: List[str]  # frontend args + program path (after --no-cache)
    stdin_path: Optional[Path] = None
    skip_reason: Optional[str] = None

    @property
    def unit_id(self) -> str:
        return f"{self.suite}/{self.name}"


@dataclass
class RunResult:
    exit_code: Optional[int]
    stdout: bytes
    stderr: bytes
    timed_out: bool
    duration: float


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# Corpus discovery
# ---------------------------------------------------------------------------

def discover_pascal(include_net: bool) -> List[Unit]:
    tests_dir = REPO_ROOT / "components" / "pascal" / "tests"
    fixtures_dir = tests_dir / "Pascal"
    units: List[Unit] = []
    for path in sorted(fixtures_dir.iterdir()):
        if not path.is_file() or "." in path.name:
            continue
        name = path.name
        unit = Unit(
            suite="pascal",
            name=name,
            frontend="pascal",
            cwd=tests_dir,
            argv_tail=[f"Pascal/{name}"],
        )
        stdin_path = fixtures_dir / f"{name}.in"
        if stdin_path.exists():
            unit.stdin_path = stdin_path
        if (fixtures_dir / f"{name}.net").exists() and not include_net:
            unit.skip_reason = "network fixture (--include-net to run)"
        units.append(unit)
    return units


def discover_clike(include_net: bool) -> List[Unit]:
    tests_dir = REPO_ROOT / "components" / "clike" / "tests"
    units: List[Unit] = []
    for path in sorted(tests_dir.glob("*.cl")):
        name = path.stem
        unit = Unit(
            suite="clike",
            name=name,
            frontend="clike",
            cwd=tests_dir,
            argv_tail=[path.name],
        )
        stdin_path = tests_dir / f"{name}.in"
        if stdin_path.exists():
            unit.stdin_path = stdin_path
        if (tests_dir / f"{name}.net").exists() and not include_net:
            unit.skip_reason = "network fixture (--include-net to run)"
        units.append(unit)
    return units


def discover_rea(include_net: bool) -> List[Unit]:
    tests_dir = REPO_ROOT / "components" / "rea" / "tests"
    fixtures_dir = tests_dir / "rea"
    units: List[Unit] = []
    for path in sorted(fixtures_dir.glob("*.rea")):
        name = path.stem
        args_file = fixtures_dir / f"{name}.args"
        if args_file.exists():
            # Suite convention: whitespace-split contents are frontend args;
            # an empty file means "run plainly".
            extra = args_file.read_text().split()
        else:
            # Fixtures without .args are disassembly-only in the real suite.
            extra = ["--dump-bytecode-only"]
        unit = Unit(
            suite="rea",
            name=name,
            frontend="rea",
            cwd=tests_dir,
            argv_tail=extra + [f"rea/{name}.rea"],
        )
        stdin_path = fixtures_dir / f"{name}.in"
        if stdin_path.exists():
            unit.stdin_path = stdin_path
        if (fixtures_dir / f"{name}.net").exists() and not include_net:
            unit.skip_reason = "network fixture (--include-net to run)"
        units.append(unit)
    return units


def discover_aether() -> List[Unit]:
    tests_dir = REPO_ROOT / "components" / "aether" / "tests"
    # The aether suite runs from examples/showcase so its cwd-relative
    # fixture data resolves; fixtures are passed by absolute path.
    cwd = REPO_ROOT / "components" / "aether" / "examples" / "showcase"
    units: List[Unit] = []
    for path in sorted(tests_dir.glob("*.aether")):
        units.append(
            Unit(
                suite="aether",
                name=path.stem,
                frontend="aether",
                cwd=cwd,
                argv_tail=[str(path)],
            )
        )
    return units


def _walk_for_generations(node, found: Dict[str, str]) -> None:
    """Recursively collect generated_ok source_code strings keyed by sha."""
    if isinstance(node, dict):
        src = node.get("source_code")
        if isinstance(src, str) and src.strip() and node.get("generated_ok"):
            found[sha256(src.encode("utf-8"))] = src
        for value in node.values():
            _walk_for_generations(value, found)
    elif isinstance(node, list):
        for value in node:
            _walk_for_generations(value, found)


def discover_docbench(results_dir: Path, sample: int, seed: int, out_dir: Path) -> List[Unit]:
    found: Dict[str, str] = {}
    json_files = sorted(results_dir.rglob("*.json")) if results_dir.exists() else []
    for jf in json_files:
        try:
            data = json.loads(jf.read_text(encoding="utf-8", errors="replace"))
        except (json.JSONDecodeError, OSError):
            continue
        _walk_for_generations(data, found)
    hashes = sorted(found)
    if sample and len(hashes) > sample:
        hashes = sorted(random.Random(seed).sample(hashes, sample))
    src_dir = out_dir / "docbench_src"
    run_cwd = out_dir / "docbench_cwd"
    src_dir.mkdir(parents=True, exist_ok=True)
    run_cwd.mkdir(parents=True, exist_ok=True)
    units: List[Unit] = []
    for digest in hashes:
        name = digest[:12]
        src_path = src_dir / f"{name}.ae"
        if not src_path.exists():
            src_path.write_text(found[digest], encoding="utf-8")
        units.append(
            Unit(
                suite="docbench",
                name=name,
                frontend="aether",
                cwd=run_cwd,
                argv_tail=[str(src_path)],
            )
        )
    return units


def discover_units(args, out_dir: Path) -> List[Unit]:
    units: List[Unit] = []
    if "pascal" in args.suites:
        units += discover_pascal(args.include_net)
    if "clike" in args.suites:
        units += discover_clike(args.include_net)
    if "rea" in args.suites:
        units += discover_rea(args.include_net)
    if "aether" in args.suites:
        units += discover_aether()
    if "docbench" in args.suites:
        units += discover_docbench(
            Path(args.docbench_results), args.docbench_sample, args.docbench_seed, out_dir
        )

    for unit in units:
        if unit.skip_reason is None and unit.unit_id in BUILTIN_SKIP:
            unit.skip_reason = BUILTIN_SKIP[unit.unit_id]

    if args.only:
        units = [u for u in units if any(pat in u.unit_id for pat in args.only)]
    if args.limit:
        units = units[: args.limit]
    return units


# ---------------------------------------------------------------------------
# Execution and comparison
# ---------------------------------------------------------------------------

def resolve_binaries(root: str) -> Dict[str, Path]:
    base = Path(root).expanduser().resolve()
    binaries: Dict[str, Path] = {}
    for frontend in FRONTENDS:
        for candidate in (base / frontend, base / "bin" / frontend):
            if candidate.is_file() and os.access(candidate, os.X_OK):
                binaries[frontend] = candidate
                break
        else:
            raise FileNotFoundError(
                f"no executable '{frontend}' under {base} (looked in . and bin/)"
            )
    return binaries


def build_env() -> Dict[str, str]:
    env = dict(os.environ)
    # Match the suites: headless SDL unless explicitly opted into real video.
    if env.get("RUN_SDL", "0") != "1":
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
    return env


def run_program(binary: Path, unit: Unit, timeout: float, env: Dict[str, str]) -> RunResult:
    cmd = [str(binary), "--no-cache", *unit.argv_tail]
    stdin_file = open(unit.stdin_path, "rb") if unit.stdin_path else subprocess.DEVNULL
    start = time.monotonic()
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=unit.cwd,
            stdin=stdin_file,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            start_new_session=True,  # so a timeout can kill the whole group
        )
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            stdout, stderr = proc.communicate()
        return RunResult(
            exit_code=None if timed_out else proc.returncode,
            stdout=stdout,
            stderr=stderr,
            timed_out=timed_out,
            duration=time.monotonic() - start,
        )
    finally:
        if unit.stdin_path:
            stdin_file.close()


def runs_equal(a: RunResult, b: RunResult) -> bool:
    if a.timed_out and b.timed_out:
        # Kill-time output truncation is racy; a double timeout is treated
        # as equivalent (reported separately as MATCH_TIMEOUT).
        return True
    if a.timed_out != b.timed_out:
        return False
    return a.exit_code == b.exit_code and a.stdout == b.stdout and a.stderr == b.stderr


def run_summary(r: RunResult) -> dict:
    return {
        "exit_code": r.exit_code,
        "timed_out": r.timed_out,
        "duration_s": round(r.duration, 3),
        "stdout_sha256": sha256(r.stdout),
        "stdout_bytes": len(r.stdout),
        "stderr_sha256": sha256(r.stderr),
        "stderr_bytes": len(r.stderr),
    }


def write_artifact(path: Path, data: bytes) -> None:
    truncated = data[:ARTIFACT_CAP]
    path.write_bytes(truncated)
    if len(data) > ARTIFACT_CAP:
        with path.open("ab") as fh:
            fh.write(b"\n[vm_diff_harness: artifact truncated at %d of %d bytes]\n"
                     % (ARTIFACT_CAP, len(data)))


def evaluate_unit(
    unit: Unit,
    bins_a: Dict[str, Path],
    bins_b: Dict[str, Path],
    timeout: float,
    env: Dict[str, str],
    diffs_dir: Path,
) -> dict:
    record: dict = {
        "unit": unit.unit_id,
        "frontend": unit.frontend,
        "cwd": str(unit.cwd),
        "argv_tail": unit.argv_tail,
        "stdin": str(unit.stdin_path) if unit.stdin_path else None,
        "timeout_s": timeout,
    }
    if unit.skip_reason:
        record.update(status="SKIP", reason=unit.skip_reason)
        return record

    bin_a = bins_a[unit.frontend]
    bin_b = bins_b[unit.frontend]
    a1 = run_program(bin_a, unit, timeout, env)
    b1 = run_program(bin_b, unit, timeout, env)
    record["a"] = run_summary(a1)
    record["b"] = run_summary(b1)

    if runs_equal(a1, b1):
        record["status"] = "MATCH_TIMEOUT" if a1.timed_out else "MATCH"
        return record

    # Divergence candidate: re-run each side once.  A side that disagrees
    # with itself makes the unit NONDET, not a diff.
    a2 = run_program(bin_a, unit, timeout, env)
    b2 = run_program(bin_b, unit, timeout, env)
    record["a_rerun"] = run_summary(a2)
    record["b_rerun"] = run_summary(b2)
    if not runs_equal(a1, a2) or not runs_equal(b1, b2):
        record["status"] = "NONDET"
        return record
    if runs_equal(a2, b2):
        # Stable per side yet the pairs disagree across rounds: order/state
        # dependent behaviour.  Classify as NONDET rather than a VM diff.
        record["status"] = "NONDET"
        record["note"] = "cross-round instability (state-dependent fixture)"
        return record

    record["status"] = "DIFF"
    unit_diff_dir = diffs_dir / unit.suite / unit.name
    unit_diff_dir.mkdir(parents=True, exist_ok=True)
    write_artifact(unit_diff_dir / "a.stdout", a2.stdout)
    write_artifact(unit_diff_dir / "a.stderr", a2.stderr)
    write_artifact(unit_diff_dir / "b.stdout", b2.stdout)
    write_artifact(unit_diff_dir / "b.stderr", b2.stderr)
    meta = {
        "unit": unit.unit_id,
        "cmd_a": [str(bin_a), "--no-cache", *unit.argv_tail],
        "cmd_b": [str(bin_b), "--no-cache", *unit.argv_tail],
        "cwd": str(unit.cwd),
        "stdin": str(unit.stdin_path) if unit.stdin_path else None,
        "a": run_summary(a2),
        "b": run_summary(b2),
    }
    (unit_diff_dir / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    record["diff_dir"] = str(unit_diff_dir)
    return record


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def result_path(out_dir: Path, unit: Unit) -> Path:
    return out_dir / "results" / unit.suite / f"{unit.name}.json"


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Differential harness: run a corpus through two pscal VM "
        "builds and byte-compare stdout/stderr/exit codes.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--vm-a", required=True,
                        help="First build: a bin dir or build tree containing bin/.")
    parser.add_argument("--vm-b", required=True,
                        help="Second build: a bin dir or build tree containing bin/.")
    parser.add_argument("--suites", default=",".join(DEFAULT_SUITES),
                        help=f"Comma-separated subset of: {','.join(ALL_SUITES)}.")
    parser.add_argument("--out", default=str(REPO_ROOT / "Tests" / "vm_diff_out" / "default"),
                        help="Output/state directory (per-unit results, diff artifacts).")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="Per-run timeout in seconds (suites use 25).")
    parser.add_argument("--only", action="append", default=[],
                        help="Substring filter on unit ids (repeatable).")
    parser.add_argument("--limit", type=int, default=0,
                        help="Run at most N units (0 = no limit).")
    parser.add_argument("--list", action="store_true",
                        help="List selected unit ids and exit.")
    parser.add_argument("--rerun", choices=["none", "bad", "all"], default="none",
                        help="none: skip units with recorded results (resume); "
                        "bad: re-run recorded DIFF/NONDET/MATCH_TIMEOUT/ERROR units; "
                        "all: re-run everything.")
    parser.add_argument("--include-net", action="store_true",
                        help="Include fixtures with .net markers.")
    parser.add_argument("--docbench-sample", type=int, default=100,
                        help="Max generated programs sampled for the docbench suite.")
    parser.add_argument("--docbench-seed", type=int, default=20260704,
                        help="Sampling seed for the docbench suite.")
    parser.add_argument("--docbench-results",
                        default=str(REPO_ROOT / "Tests" / "aether_doc_bench" / "results"),
                        help="Directory scanned (recursively) for bench result JSONs.")
    args = parser.parse_args(argv)

    args.suites = [s.strip() for s in args.suites.split(",") if s.strip()]
    unknown = [s for s in args.suites if s not in ALL_SUITES]
    if unknown:
        parser.error(f"unknown suite(s): {', '.join(unknown)} (valid: {', '.join(ALL_SUITES)})")

    out_dir = Path(args.out).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    units = discover_units(args, out_dir)
    if args.list:
        for unit in units:
            suffix = f"  [skip: {unit.skip_reason}]" if unit.skip_reason else ""
            print(f"{unit.unit_id}{suffix}")
        print(f"\n{len(units)} unit(s) selected.")
        return 0
    if not units:
        print("No units selected.", file=sys.stderr)
        return 2

    try:
        bins_a = resolve_binaries(args.vm_a)
        bins_b = resolve_binaries(args.vm_b)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    env = build_env()
    diffs_dir = out_dir / "diffs"
    progress_log = out_dir / "progress.log"

    print(f"vm-a: {Path(args.vm_a).resolve()}")
    print(f"vm-b: {Path(args.vm_b).resolve()}")
    print(f"out:  {out_dir}")
    print(f"{len(units)} unit(s) selected; timeout {args.timeout}s per run.\n")

    records: List[dict] = []
    ran = 0
    for index, unit in enumerate(units, 1):
        rpath = result_path(out_dir, unit)
        record: Optional[dict] = None
        if rpath.exists() and args.rerun != "all":
            try:
                record = json.loads(rpath.read_text())
            except (json.JSONDecodeError, OSError):
                record = None
            if record is not None and args.rerun == "bad" and record.get("status") not in ("MATCH", "SKIP"):
                record = None
        cached = record is not None
        if record is None:
            try:
                record = evaluate_unit(unit, bins_a, bins_b, args.timeout, env, diffs_dir)
            except Exception as exc:  # harness bug or environment failure
                record = {"unit": unit.unit_id, "status": "ERROR", "error": repr(exc)}
            rpath.parent.mkdir(parents=True, exist_ok=True)
            tmp = rpath.with_suffix(".json.tmp")
            tmp.write_text(json.dumps(record, indent=2) + "\n")
            tmp.replace(rpath)
            ran += 1
        records.append(record)
        status = record["status"]
        tag = " (cached)" if cached else ""
        dur = ""
        if not cached and "a" in record:
            dur = f" [{record['a']['duration_s']}s/{record['b']['duration_s']}s]"
        line = f"[{index}/{len(units)}] {status:<13} {unit.unit_id}{dur}{tag}"
        print(line, flush=True)
        with progress_log.open("a") as fh:
            fh.write(line + "\n")

    counts: Dict[str, int] = {}
    for record in records:
        counts[record["status"]] = counts.get(record["status"], 0) + 1
    diffs = [r for r in records if r["status"] == "DIFF"]
    errors = [r for r in records if r["status"] == "ERROR"]
    nondet = [r for r in records if r["status"] == "NONDET"]
    timeouts = [r for r in records if r["status"] == "MATCH_TIMEOUT"]

    summary = {
        "vm_a": str(Path(args.vm_a).resolve()),
        "vm_b": str(Path(args.vm_b).resolve()),
        "suites": args.suites,
        "units": len(records),
        "ran_this_invocation": ran,
        "counts": counts,
        "diff_units": [r["unit"] for r in diffs],
        "error_units": [r["unit"] for r in errors],
        "nondet_units": [r["unit"] for r in nondet],
        "timeout_units": [r["unit"] for r in timeouts],
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    print("\nSummary:")
    for status in ("MATCH", "SKIP", "MATCH_TIMEOUT", "NONDET", "DIFF", "ERROR"):
        if counts.get(status):
            print(f"  {status}: {counts[status]}")
    for label, group in (("DIFF", diffs), ("ERROR", errors), ("NONDET", nondet),
                         ("MATCH_TIMEOUT", timeouts)):
        for record in group:
            extra = f"  -> {record['diff_dir']}" if record.get("diff_dir") else ""
            print(f"  {label}: {record['unit']}{extra}")

    if diffs or errors:
        print("\nFAIL: divergences or harness errors recorded.")
        return 1
    print("\nPASS: no divergences between the two builds.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
