#!/usr/bin/env python3
"""Manifest-driven regression harness for the PSCAL shell front end."""

from __future__ import annotations

import argparse
import json
import os
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional

HARNESS_ROOT = Path(__file__).resolve().parent
REPO_ROOT = HARNESS_ROOT.parent.parent
DEFAULT_MANIFEST = HARNESS_ROOT / "tests" / "manifest.json"
DEFAULT_TIMEOUT = 20.0


@dataclass
class TestCase:
    test_id: str
    name: str
    category: str
    description: str
    script: Path
    expect: str
    args: List[str]
    env: Dict[str, str]
    expected_stdout: Optional[str]
    expected_stderr_substring: Optional[str]
    prime_cache: bool


@dataclass
class TestResult:
    case: TestCase
    passed: bool
    reason: Optional[str] = None
    stdout: str = ""
    stderr: str = ""
    returncode: Optional[int] = None


def load_manifest(path: Path) -> List[TestCase]:
    data = json.loads(path.read_text(encoding="utf-8"))
    tests = []
    for raw in data.get("tests", []):
        script_path = (REPO_ROOT / raw["script"]).resolve()
        tests.append(
            TestCase(
                test_id=raw["id"],
                name=raw.get("name", raw["id"]),
                category=raw.get("category", "misc"),
                description=raw.get("description", ""),
                script=script_path,
                expect=raw.get("expect", "runtime_ok"),
                args=list(raw.get("args", [])),
                env=dict(raw.get("env", {})),
                expected_stdout=raw.get("expected_stdout"),
                expected_stderr_substring=raw.get("expected_stderr_substring"),
                prime_cache=bool(raw.get("prime_cache", False)),
            )
        )
    return tests


def ensure_executable() -> Path:
    exe = REPO_ROOT / "build" / "bin" / "exsh"
    if not exe.exists():
        raise FileNotFoundError(f"exsh executable not found at {exe}; build the project first")
    return exe


def run_exsh(executable: Path, case: TestCase, extra_args: Optional[List[str]] = None) -> subprocess.CompletedProcess[str]:
    cmd = [str(executable)]
    if extra_args:
        cmd.extend(extra_args)
    cmd.append(str(case.script))
    env = os.environ.copy()
    env.update(case.env)
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        text=True,
        capture_output=True,
        timeout=DEFAULT_TIMEOUT,
        env=env,
    )


def evaluate(case: TestCase, proc: subprocess.CompletedProcess[str]) -> TestResult:
    expect_ok = case.expect == "runtime_ok"
    passed = (proc.returncode == 0) if expect_ok else (proc.returncode != 0)
    reason = None

    stdout_text = proc.stdout
    stderr_text = proc.stderr

    if case.expected_stdout is not None:
        if stdout_text.strip() != case.expected_stdout.strip():
            passed = False
            reason = (
                reason or
                "stdout mismatch"
            )
    if case.expected_stderr_substring:
        if case.expected_stderr_substring not in stderr_text:
            passed = False
            reason = reason or "missing stderr substring"

    if not passed and reason is None:
        reason = f"unexpected return code {proc.returncode}"

    return TestResult(case=case, passed=passed, reason=reason, stdout=stdout_text, stderr=stderr_text, returncode=proc.returncode)


def run_case(executable: Path, case: TestCase) -> TestResult:
    if not case.script.exists():
        return TestResult(case=case, passed=False, reason=f"script not found: {case.script}")

    if case.prime_cache:
        try:
            run_exsh(executable, case, extra_args=["--no-cache"])
        except subprocess.SubprocessError:
            pass  # ignore priming errors; actual run will report details

    proc = run_exsh(executable, case)
    return evaluate(case, proc)


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run shell front-end regression tests")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Path to test manifest")
    parser.add_argument("--only", type=str, default=None, help="Run only tests whose id contains this substring")
    parser.add_argument("--list", action="store_true", help="List available tests and exit")
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    tests = load_manifest(args.manifest)
    if args.only:
        tests = [t for t in tests if args.only in t.test_id]
    if args.list:
        for case in tests:
            print(f"{case.test_id}: {case.name} ({case.category})")
            if case.description:
                print(f"  {case.description}")
        return 0

    if not tests:
        print("No tests selected", file=sys.stderr)
        return 1

    try:
        executable = ensure_executable()
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    results = [run_case(executable, case) for case in tests]

    failures = [r for r in results if not r.passed]
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        print(f"[{status}] {result.case.test_id} – {result.case.name}")
        if not result.passed and result.reason:
            print(f"    Reason: {result.reason}")
            if result.stdout:
                print("    stdout:\n" + "\n".join(f"        {line}" for line in result.stdout.strip().splitlines()))
            if result.stderr:
                print("    stderr:\n" + "\n".join(f"        {line}" for line in result.stderr.strip().splitlines()))

    print()
    print(f"Ran {len(results)} shell test(s); {len(failures)} failure(s)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
