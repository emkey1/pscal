#!/usr/bin/env python3
"""Scope conformance harness for the Pascal front-end."""

from __future__ import annotations

import argparse
import csv
import json
import shlex
import subprocess
import sys
import textwrap
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional

HARNESS_ROOT = Path(__file__).resolve().parent


def locate_repo_root(start: Path) -> Path:
    for candidate in [start] + list(start.parents):
        if (candidate / "CMakeLists.txt").exists() or (candidate / ".git").exists():
            return candidate
    return start


REPO_ROOT = locate_repo_root(HARNESS_ROOT)
DEFAULT_MANIFEST = HARNESS_ROOT / "tests" / "manifest.json"
DEFAULT_OUT_DIR = HARNESS_ROOT / "out"
DEFAULT_TIMEOUT = 20.0
CSV_HEADER = ["name", "category", "expect", "exit_code", "pass", "reason"]


def derive_default_command() -> str:
    exe = (REPO_ROOT / "build" / "bin" / "pascal").resolve()
    return f"{shlex.quote(str(exe))} {{source}}"


@dataclass
class TestCase:
    test_id: str
    name: str
    category: str
    expect: str
    code: str
    extension: str
    expected_stdout: Optional[str] = None
    expected_stderr: Optional[str] = None


@dataclass
class TestResult:
    case: TestCase
    passed: bool
    exit_code: int
    stdout: str
    stderr: str
    reason: Optional[str]
    source_path: Path


class HarnessError(RuntimeError):
    """Raised when the manifest or configuration is invalid."""


def normalize_output(text: str) -> str:
    return text.replace("\r\n", "\n").rstrip()


def load_manifest(path: Path) -> tuple[List[TestCase], str]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise HarnessError(f"Manifest not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise HarnessError(f"Failed to parse manifest: {exc}") from exc

    tests_raw = data.get("tests")
    if not isinstance(tests_raw, list):
        raise HarnessError("Manifest must contain a list under the 'tests' key")

    default_ext = data.get("default_extension", "pas")
    if not isinstance(default_ext, str) or not default_ext:
        default_ext = "pas"

    cases: List[TestCase] = []
    for entry in tests_raw:
        missing = [key for key in ("id", "name", "category", "code", "expect") if key not in entry]
        if missing:
            raise HarnessError(f"Test entry missing required keys: {', '.join(missing)}")
        expect = entry["expect"]
        if expect not in {"compile_ok", "compile_error", "runtime_ok", "runtime_error"}:
            raise HarnessError(f"Test {entry['id']} has unsupported expect '{expect}'")
        code = textwrap.dedent(entry["code"]).strip("\n") + "\n"
        extension = entry.get("extension", default_ext)
        if not isinstance(extension, str) or not extension:
            extension = default_ext
        cases.append(
            TestCase(
                test_id=entry["id"],
                name=entry["name"],
                category=entry["category"],
                expect=expect,
                code=code,
                extension=extension,
                expected_stdout=entry.get("expected_stdout"),
                expected_stderr=entry.get("expected_stderr_substring"),
            )
        )
    return cases, default_ext


def matches_filter(case: TestCase, filters: Optional[Iterable[str]]) -> bool:
    if not filters:
        return True
    lowered = [f.lower() for f in filters]
    target = f"{case.test_id} {case.category} {case.name}".lower()
    return any(f in target for f in lowered)


def materialise_case(case: TestCase, out_root: Path) -> Path:
    case_dir = out_root / case.category / case.test_id
    case_dir.mkdir(parents=True, exist_ok=True)
    source_path = case_dir / f"main.{case.extension}"
    source_path.write_text(case.code, encoding="utf-8")
    return source_path


def run_case(case: TestCase, command_template: str, out_root: Path, timeout: float) -> TestResult:
    source_path = materialise_case(case, out_root)
    command = command_template.format(source=shlex.quote(str(source_path)))
    try:
        completed = subprocess.run(
            command,
            shell=True,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=str(REPO_ROOT),
        )
    except subprocess.TimeoutExpired:
        return TestResult(
            case=case,
            passed=False,
            exit_code=-1,
            stdout="",
            stderr="",
            reason="timeout",
            source_path=source_path,
        )

    stdout = normalize_output(completed.stdout)
    stderr = normalize_output(completed.stderr)
    exit_code = completed.returncode

    passed = True
    reason: Optional[str] = None

    if case.expect in {"compile_ok", "runtime_ok"}:
        if exit_code != 0:
            passed = False
            reason = f"expected exit 0 but got {exit_code}"
        elif case.expected_stdout is not None and stdout != normalize_output(case.expected_stdout):
            passed = False
            reason = "stdout mismatch"
        elif case.expected_stderr is not None and case.expected_stderr not in stderr:
            passed = False
            reason = "missing expected stderr"
    elif case.expect in {"compile_error", "runtime_error"}:
        if exit_code == 0:
            passed = False
            reason = "expected non-zero exit code"
        elif case.expected_stderr is not None and case.expected_stderr not in stderr:
            passed = False
            reason = "missing expected stderr"
    else:
        passed = False
        reason = f"unsupported expectation {case.expect}"

    return TestResult(
        case=case,
        passed=passed,
        exit_code=exit_code,
        stdout=stdout,
        stderr=stderr,
        reason=reason,
        source_path=source_path,
    )


def list_cases(cases: Iterable[TestCase]) -> None:
    for case in cases:
        print(f"{case.test_id}: {case.name} ({case.category})")


def write_csv(results: Iterable[TestResult], path: Path) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(CSV_HEADER)
        for result in results:
            writer.writerow(
                [
                    result.case.name,
                    result.case.category,
                    result.case.expect,
                    result.exit_code,
                    "pass" if result.passed else "fail",
                    result.reason or "",
                ]
            )


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Pascal scope verification harness")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Path to manifest.json")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help="Directory for generated sources")
    parser.add_argument("--cmd", default=derive_default_command(), help="Command template used to run tests")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="Per-test timeout in seconds")
    parser.add_argument("--only", action="append", help="Run only tests whose id/name/category match the filter")
    parser.add_argument("--list", action="store_true", help="List tests and exit")
    parser.add_argument("--csv", type=Path, help="Optional CSV summary path")

    args = parser.parse_args(argv)

    try:
        cases, _ = load_manifest(args.manifest)
    except HarnessError as exc:
        print(f"Harness configuration error: {exc}", file=sys.stderr)
        return 2

    filtered = [case for case in cases if matches_filter(case, args.only)]

    if args.list:
        list_cases(filtered)
        return 0

    if not filtered:
        print("No tests matched the provided filters.", file=sys.stderr)
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)

    results: List[TestResult] = []
    for case in filtered:
        result = run_case(case, args.cmd, args.out_dir, args.timeout)
        status = "PASS" if result.passed else "FAIL"
        print(f"[{status}] {case.test_id} ({case.category})")
        if not result.passed and result.reason:
            print(f"    Reason: {result.reason}")
            if result.stderr:
                print(textwrap.indent(result.stderr, "    stderr: "))
        results.append(result)

    if args.csv:
        write_csv(results, args.csv)

    passed = sum(1 for r in results if r.passed)
    total = len(results)
    print(f"\nSummary: {passed}/{total} tests passed")

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
