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
              harness's --deny net,proc (the 2026-10-06 sandbox fix).

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

# Opens a socket inside fx. Under --deny net the VM must refuse it; if it ever
# prints its line, the sandbox is not containing generated code.
SANDBOX_PROBE = """fn main() -> Void {
    fx {
        let s: Int = socketcreate(0);
        println("socket created: ", s);
    }
}
"""


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
    return problems


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
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--report-json", type=pathlib.Path)
    ap.add_argument("--quiet", action="store_true", help="print failures and the totals only")
    args = ap.parse_args(argv)

    manifests = args.tasks or default_manifests()
    report: dict[str, Any] = {"manifests": [str(adb.display_path(m)) for m in manifests]}
    lint = [problem for m in manifests for problem in lint_manifest(m)]
    report["lint_problems"] = lint
    for problem in lint:
        print(f"[LINT] {problem}")
    if args.lint_only:
        print(f"lint: {len(manifests)} manifests, {len(lint)} problem(s)")
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
        elapsed = time.time() - started
        report["elapsed_seconds"] = round(elapsed, 2)
        report["failures"] = failures
        summary = [f"aether {toolchain['aether_version']} sha256 {toolchain['binary_sha256'][:16]}"]
        if not args.negatives_only:
            summary.append(f"references {report['references_passed']}/{report['references_total']}")
            summary.append(f"sandbox {'ok' if report['sandbox_probe']['ok'] else 'FAILED'}")
        summary.append(f"negatives {sum(n['ok'] for n in negatives)}/{len(negatives)}")
        print(f"\noracle: {', '.join(summary)} in {elapsed:.1f}s -- {failures} failure(s)")
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)
    if args.report_json:
        args.report_json.write_text(json.dumps(report, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
