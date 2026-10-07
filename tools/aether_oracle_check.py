#!/usr/bin/env python3
"""Check the benchmark's oracle against an aether binary: the oracle lap.

Three checks, all through the harness's own compile_and_run and
materialize_task_files (so the sandbox flags, cwd and task files are exactly
what a scored case gets), on a hashed private copy of the binary:

  references  every `reference_solution` must print its task's expected stdout
              byte-exact (and exit with its expected_returncode);
  negatives   every `should_fail` program must be rejected, emitting its
              `expected_error_code`;
  sandbox     a program that opens a socket must be rejected under the
              harness's --deny net,proc (the 2026-10-06 sandbox fix);
  python      every Tests/aether_doc_bench/py_refs/<task_id>.py (the
              independent oracle, W1-11/W1-22) must print the same expected
              stdout and exit status, through the harness's python lane. A
              compiler semantics bug can no longer become oracle truth by
              being the only thing that checks a reference.

With --traps it also runs each trap suite task's `trap.natural_program` (the
plausible program the trap exists to catch) and reports its failure class on
this binary -- silent_wrong while the language bug is open, pass once a fix
lands. That report is informational: a trap that stops firing is the fix
being measured, not an oracle failure.

Nothing re-checked the oracle when the language changed: 9 references had
gone stale on FX-001, and algo_sliding_window_max had been "verified by
running" a reference that depended on the compiler re-reading a shrinking
range bound. aether_doc_bench.py runs the reference and sandbox checks as a
pre-flight for the tasks it is about to score; this tool runs all of them, for
tools/instrument_check.sh and by hand.

  python3 tools/aether_oracle_check.py --aether-bin build/bin/aether
  python3 tools/aether_oracle_check.py --lint-only     # no binary: manifests only

Exit 0 when every check passes, 1 otherwise. (This grew out of
tools/aether_negative_tier.py, which ran the negative tier alone;
`--negatives-only` keeps that behaviour.)
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import pathlib
import shutil
import sys
import tempfile
import time
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_doc_bench as adb  # noqa: E402

BENCH_DIR = adb.REPO_ROOT / "Tests" / "aether_doc_bench"
PY_REFS_DIR = BENCH_DIR / "py_refs"
# Effects a task may take back from the sandbox (sandbox_allow). Never net:
# the sandbox exists to keep generated code off the network.
ALLOWED_SANDBOX_ALLOW = frozenset({"proc"})

# Opens a socket inside fx. Under --deny net the VM must refuse it; if it ever
# prints its line, the sandbox is not containing generated code.
SANDBOX_PROBE = """fn main() -> Void {
    fx {
        let s: Int = socketcreate(0);
        println("socket created: ", s);
    }
}
"""


# Suites whose references take minutes (the scale tier's 5 MB TOON rollup is
# quadratic today): linted always, run only when named with --tasks or with
# --include-slow, so the default lap stays a pre-commit-sized check.
SLOW_MANIFESTS = frozenset({"tasks_scale.json"})


def default_manifests() -> list[pathlib.Path]:
    return sorted(BENCH_DIR.glob("tasks*.json")) + [BENCH_DIR / "smoke_tasks.json"]


def lint_manifest(path: pathlib.Path) -> list[str]:
    """Problems a manifest has without running anything: it must load (the
    --list-tasks path), ids must be unique, negatives must carry their program
    and code."""
    problems: list[str] = []
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
        tasks = adb.load_tasks(path)
    except (Exception, SystemExit) as exc:  # noqa: BLE001
        return [f"{path.name}: does not load: {exc}"]
    ids = [t.task_id for t in tasks]
    for dup in sorted({i for i in ids if ids.count(i) > 1}):
        problems.append(f"{path.name}: duplicate task id {dup}")
    for item in raw.get("tasks", []):
        if item.get("should_fail") and not (item.get("program") and item.get("expected_error_code")):
            problems.append(f"{path.name}:{item.get('id')}: should_fail needs program and expected_error_code")
        bad_allow = sorted(set(item.get("sandbox_allow") or ()) - ALLOWED_SANDBOX_ALLOW)
        if bad_allow:
            problems.append(f"{path.name}:{item.get('id')}: sandbox_allow may only name "
                            f"{sorted(ALLOWED_SANDBOX_ALLOW)}, not {bad_allow}")
        for name, spec in (item.get("generated_files") or {}).items():
            if not (isinstance(spec, dict) and spec.get("generator") and spec.get("sha256")):
                problems.append(f"{path.name}:{item.get('id')}: generated file {name!r} needs generator and sha256")
        if "trap" in item:
            if not (item["trap"] or {}).get("natural_program"):
                problems.append(f"{path.name}:{item.get('id')}: a trap needs trap.natural_program")
            if not (item.get("hide_expected_stdout", raw.get("hide_expected_stdout"))):
                problems.append(f"{path.name}:{item.get('id')}: a trap task must hide its expected stdout")
            if not python_reference_path(item["id"]).is_file():
                problems.append(f"{path.name}:{item.get('id')}: a trap needs py_refs/{item['id']}.py")
    return problems


def python_reference_path(task_id: str) -> pathlib.Path:
    return PY_REFS_DIR / f"{task_id}.py"


def lint_python_references(manifests: list[pathlib.Path]) -> list[str]:
    """Every py_refs/<id>.py must belong to a task in some manifest."""
    if not PY_REFS_DIR.is_dir():
        return []
    ids: set[str] = set()
    for manifest in default_manifests():
        try:
            ids.update(t.task_id for t in adb.load_tasks(manifest))
        except (Exception, SystemExit):  # noqa: BLE001 -- lint_manifest reports it
            pass
    return [f"py_refs/{p.name}: no task {p.stem!r} in any manifest"
            for p in sorted(PY_REFS_DIR.glob("*.py")) if p.stem not in ids]


def check_python_reference(task: adb.Task) -> dict[str, Any] | None:
    """The independent oracle: py_refs/<id>.py through the python lane. None
    when the task has no Python reference."""
    path = python_reference_path(task.task_id)
    if not path.is_file():
        return None
    run = adb.run_python_task(task, path.read_text(encoding="utf-8"))
    ok = bool(run["exact_stdout_match"])
    detail = ""
    if not ok:
        if run.get("timed_out"):
            detail = f"timeout after {task.timeout_seconds} s"
        elif run["returncode"] != task.expected_returncode:
            detail = f"rc={run['returncode']} {(run['stderr'].strip().splitlines() or [''])[-1][:200]}"
        else:
            detail = adb.describe_stdout_mismatch(task.expected_stdout, run["stdout"])[:300]
    return {"task_id": task.task_id, "ok": ok, "detail": detail, "elapsed_seconds": run["elapsed_seconds"]}


def check_python_references(tasks: list[adb.Task], workers: int = 8) -> dict[str, dict[str, Any]]:
    """{task_id: {ok, detail}} for every task with a py_refs file."""
    results: dict[str, dict[str, Any]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        for result in pool.map(check_python_reference, tasks):
            if result is not None:
                results[result.pop("task_id")] = result
    return results


def trap_items(path: pathlib.Path) -> dict[str, dict[str, Any]]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    return {item["id"]: item["trap"] for item in raw.get("tasks", []) if isinstance(item.get("trap"), dict)}


def check_traps(path: pathlib.Path, args: Any, version: str | None, workers: int = 8) -> list[dict[str, Any]]:
    """Run each trap's natural program on this binary and classify it the way
    a board classifies a first attempt."""
    traps = trap_items(path)
    tasks = [t for t in adb.load_tasks(path) if t.task_id in traps]

    def one(task: adb.Task) -> dict[str, Any]:
        trap = traps[task.task_id]
        run = adb.compile_and_run(task, trap["natural_program"], args)
        observed = adb.classify_attempt({"run": run, "generated_ok": True, "source_code": "x"})
        recorded = (trap.get("observed") or {}).get(version or "")
        return {"manifest": path.name, "id": task.task_id, "class": observed,
                "recorded": recorded, "fires": observed != "pass"}

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        return list(pool.map(one, tasks))


def check_reference(task: adb.Task, args: Any) -> dict[str, Any]:
    run = adb.compile_and_run(task, task.reference_solution or "", args)
    ok = bool(run["exact_stdout_match"])
    detail = ""
    if not ok:
        if run.get("timed_out"):
            detail = f"timeout after {task.timeout_seconds} s"
        elif run["returncode"] != task.expected_returncode:
            detail = f"rc={run['returncode']} {(run['stderr'].strip().splitlines() or [''])[0][:200]}"
        else:
            detail = adb.describe_stdout_mismatch(task.expected_stdout, run["stdout"])[:300]
    return {"task_id": task.task_id, "ok": ok, "detail": detail, "returncode": run["returncode"]}


def check_references(tasks: list[adb.Task], args: Any, workers: int = 8) -> dict[str, dict[str, Any]]:
    """{task_id: {ok, detail}} for every task with a reference (others: ok None)."""
    results: dict[str, dict[str, Any]] = {}
    with_ref = [t for t in tasks if t.reference_solution]
    for task in tasks:
        if not task.reference_solution:
            results[task.task_id] = {"ok": None, "detail": "no reference_solution"}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        for result in pool.map(lambda t: check_reference(t, args), with_ref):
            results[result["task_id"]] = {"ok": result["ok"], "detail": result["detail"]}
    return results


def check_negatives(path: pathlib.Path, args: Any) -> list[dict[str, Any]]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    out = []
    for item in raw.get("tasks", []):
        if not item.get("should_fail"):
            continue
        task = adb.Task(task_id=item["id"], title=item.get("title", ""), prompt="", expected_stdout="")
        run = adb.compile_and_run(task, item.get("program", ""), args)
        code = item.get("expected_error_code", "")
        codes = [d.get("code") for d in (run.get("diagnostics") or []) if isinstance(d, dict)]
        seen = code in (run["stdout"] + run["stderr"]) or code in codes
        ok = run["returncode"] != 0 and seen
        out.append({"manifest": path.name, "id": item["id"], "ok": ok, "expected_error_code": code,
                    "returncode": run["returncode"], "code_seen": seen})
    return out


def check_sandbox(args: Any) -> dict[str, Any]:
    """The socket probe under the harness's sandbox flags."""
    probe_args = argparse.Namespace(**vars(args))
    if "net" not in (probe_args.sandbox_deny or ""):
        probe_args.sandbox_deny = "net,proc"
    task = adb.Task(task_id="sandbox_probe", title="sandbox probe", prompt="", expected_stdout="")
    run = adb.compile_and_run(task, SANDBOX_PROBE, probe_args)
    ok = run["returncode"] != 0 and "socket created" not in run["stdout"]
    return {"ok": ok, "deny": probe_args.sandbox_deny, "returncode": run["returncode"],
            "detail": (run["stderr"].strip().splitlines() or [""])[0][:200]}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tasks", action="append", type=pathlib.Path, default=[],
                    help="manifest(s) to check (repeatable; default every Tests/aether_doc_bench manifest)")
    ap.add_argument("--aether-bin", type=pathlib.Path, default=adb.DEFAULT_AETHER_BIN)
    ap.add_argument("--aether-arg", action="append", default=[], dest="aether_args", metavar="ARG",
                    help="extra aether flag for every call, as in the harness (--aether-arg=FLAG)")
    ap.add_argument("--sandbox-deny", default="net,proc")
    ap.add_argument("--negatives-only", action="store_true", help="only the should_fail tier")
    ap.add_argument("--lint-only", action="store_true", help="only load-check the manifests (no binary)")
    ap.add_argument("--no-python", action="store_true", help="skip the py_refs (independent oracle) lap")
    ap.add_argument("--traps", action="store_true",
                    help="also run every trap's natural program and report its class (informational)")
    ap.add_argument("--include-slow", action="store_true",
                    help=f"also run the slow suites' references ({', '.join(sorted(SLOW_MANIFESTS))})")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--report-json", type=pathlib.Path)
    ap.add_argument("--quiet", action="store_true", help="print failures and the totals only")
    args = ap.parse_args(argv)

    manifests = args.tasks or default_manifests()
    report: dict[str, Any] = {"manifests": [str(adb.display_path(m)) for m in manifests]}
    lint = [problem for m in manifests for problem in lint_manifest(m)]
    lint += lint_python_references(manifests)
    if not args.tasks and not args.include_slow:
        skipped = [m for m in manifests if m.name in SLOW_MANIFESTS]
        manifests = [m for m in manifests if m.name not in SLOW_MANIFESTS]
        report["skipped_slow_manifests"] = [m.name for m in skipped]
    report["lint_problems"] = lint
    for problem in lint:
        print(f"[LINT] {problem}")
    if args.lint_only:
        print(f"lint: {len(report['manifests'])} manifests, {len(lint)} problem(s)")
        if args.report_json:
            args.report_json.write_text(json.dumps(report, indent=2))
        return 1 if lint else 0

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")
    run_dir = pathlib.Path(tempfile.mkdtemp(prefix="aether-oracle-"))
    started = time.time()
    try:
        toolchain = adb.snapshot_aether_binary(args.aether_bin, run_dir)
        args.aether_bin = toolchain.pop("path")
        args.aether_bin_display = toolchain["aether_bin"]
        args.binary_sha256 = toolchain["binary_sha256"]
        report["toolchain"] = toolchain

        failures = len(lint)
        if not args.negatives_only:
            refs_total = refs_ok = 0
            report["references"] = {}
            for manifest in manifests:
                results = check_references(adb.load_tasks(manifest), args, args.workers)
                report["references"][manifest.name] = results
                for task_id, result in sorted(results.items()):
                    if result["ok"] is None:
                        continue
                    refs_total += 1
                    refs_ok += int(result["ok"])
                    if not result["ok"]:
                        failures += 1
                        print(f"[FAIL] reference {manifest.name}:{task_id} {result['detail']}")
                    elif not args.quiet:
                        print(f"[PASS] reference {manifest.name}:{task_id}")
            report["references_passed"], report["references_total"] = refs_ok, refs_total
            if not args.no_python:
                py_total = py_ok = 0
                report["python_references"] = {}
                for manifest in manifests:
                    results = check_python_references(adb.load_tasks(manifest), args.workers)
                    report["python_references"][manifest.name] = results
                    for task_id, result in sorted(results.items()):
                        py_total += 1
                        py_ok += int(result["ok"])
                        if not result["ok"]:
                            failures += 1
                            print(f"[FAIL] python reference {manifest.name}:{task_id} {result['detail']}")
                        elif not args.quiet:
                            print(f"[PASS] python reference {manifest.name}:{task_id}")
                report["python_references_passed"], report["python_references_total"] = py_ok, py_total
            sandbox = check_sandbox(args)
            report["sandbox_probe"] = sandbox
            if not sandbox["ok"]:
                failures += 1
            print(f"[{'PASS' if sandbox['ok'] else 'FAIL'}] sandbox probe: socketcreate under --deny "
                  f"{sandbox['deny']} -> rc={sandbox['returncode']} {sandbox['detail']}")
        negatives = [n for manifest in manifests for n in check_negatives(manifest, args)]
        report["negatives"] = negatives
        for n in negatives:
            if not n["ok"]:
                failures += 1
            if not n["ok"] or not args.quiet:
                print(f"[{'PASS' if n['ok'] else 'FAIL'}] negative {n['manifest']}:{n['id']} "
                      f"rc={n['returncode']} code={n['expected_error_code']} seen={n['code_seen']}")
        if args.traps:
            traps = [t for manifest in manifests for t in check_traps(manifest, args, toolchain.get("aether_version"),
                                                                    args.workers)]
            report["traps"] = traps
            for t in traps:
                drift = "" if t["recorded"] in (None, t["class"]) else f" (recorded {t['recorded']})"
                print(f"[TRAP] {t['manifest']}:{t['id']} {t['class']}{drift}")
            fired = sum(t["fires"] for t in traps)
            silent = sum(t["class"] == "silent_wrong" for t in traps)
            print(f"traps: {fired}/{len(traps)} fire on this binary ({silent} silent_wrong)")
        elapsed = time.time() - started
        report["elapsed_seconds"] = round(elapsed, 2)
        report["failures"] = failures
        summary = [f"aether {toolchain['aether_version']} sha256 {toolchain['binary_sha256'][:16]}"]
        if not args.negatives_only:
            summary.append(f"references {report['references_passed']}/{report['references_total']}")
            if not args.no_python:
                summary.append(f"python {report['python_references_passed']}/{report['python_references_total']}")
            summary.append(f"sandbox {'ok' if report['sandbox_probe']['ok'] else 'FAILED'}")
        summary.append(f"negatives {sum(n['ok'] for n in negatives)}/{len(negatives)}")
        if report.get("skipped_slow_manifests"):
            summary.append(f"skipped {', '.join(report['skipped_slow_manifests'])} (--include-slow)")
        print(f"\noracle: {', '.join(summary)} in {elapsed:.1f}s -- {failures} failure(s)")
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)
    if args.report_json:
        args.report_json.write_text(json.dumps(report, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
