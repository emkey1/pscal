#!/usr/bin/env python3
"""Score the benchmark v2 negative-invariant tier.

These tasks (marked `should_fail: true` in tasks_v2.json) are compiler-invariant
tests, not model-generation tasks: each carries a fixed `program` that the
compiler MUST reject, plus the `expected_error_code` it must reject with. A task
passes iff the compiler exits non-zero AND emits the expected code. This is
complementary to the model-generation tasks scored by aether_doc_bench.py, and is
kept separate so the negative tier does not have to be shoehorned into that
harness's batched, model-driven loop.
"""
from __future__ import annotations
import argparse
import json
import os
import pathlib
import subprocess
import tempfile


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tasks", type=pathlib.Path,
                    default=pathlib.Path("Tests/aether_doc_bench/tasks_v2.json"))
    ap.add_argument("--aether-bin", default="build/bin/aether")
    ap.add_argument("--report-json", type=pathlib.Path)
    args = ap.parse_args()

    data = json.loads(args.tasks.read_text())
    negatives = [t for t in data.get("tasks", []) if t.get("should_fail")]
    bin_abs = os.path.abspath(args.aether_bin)

    results = []
    passed = 0
    for t in negatives:
        program = t.get("program", "")
        code = t.get("expected_error_code", "")
        with tempfile.TemporaryDirectory(prefix="aether-neg-") as td:
            path = os.path.join(td, f"{t['id']}.aether")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(program)
            proc = subprocess.run([bin_abs, "--no-cache", path], cwd=td,
                                  capture_output=True, text=True, errors="replace", timeout=30)
        rejected = proc.returncode != 0
        code_seen = code in (proc.stdout + proc.stderr)
        ok = rejected and code_seen
        passed += ok
        results.append({"id": t["id"], "ok": ok, "rejected": rejected,
                        "expected_error_code": code, "code_seen": code_seen,
                        "returncode": proc.returncode})
        print(f"[{'PASS' if ok else 'FAIL'}] {t['id']:<24} rejected={rejected} "
              f"code={code} seen={code_seen}")

    print(f"\nnegative tier: {passed}/{len(negatives)} invariants upheld")
    if args.report_json:
        args.report_json.write_text(json.dumps(
            {"passed": passed, "total": len(negatives), "results": results}, indent=2))
    return 0 if passed == len(negatives) else 1


if __name__ == "__main__":
    raise SystemExit(main())
