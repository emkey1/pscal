#!/usr/bin/env python3
"""Scope verification harness for the exsh front end.

This script consumes a manifest that describes scoped shell snippets, executes
them under the PSCAL exsh front end, optionally compares the observed behaviour
with ``bash``, and writes a human-readable summary to stdout (plus a CSV report
under ``out`` for archival purposes).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


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
IGNORED_PARITY_STDERR_PREFIXES = (
    "Compilation successful.",
    "Loaded cached byte code",
)


@dataclass
class TestCase:
    test_id: str
    name: str
    category: str
    description: str
    script: Path
    expect: str
    args: List[str]
    bash_args: List[str]
    env: Dict[str, str]
    stdin: Optional[str]
    expected_stdout: Optional[str]
    expected_stderr_substring: Optional[str]

    def uses_bash_parity(self) -> bool:
        return self.expect == "match_bash"


@dataclass
class TestResult:
    case: TestCase
    passed: bool
    reason: Optional[str]
    exsh_stdout: str
    exsh_stderr: str
    exsh_returncode: int
    bash_stdout: Optional[str] = None
    bash_stderr: Optional[str] = None
    bash_returncode: Optional[int] = None


def derive_default_exsh_command() -> List[str]:
    exe = (REPO_ROOT / "build" / "bin" / "exsh").resolve()
    return [str(exe), "{source}"]


def derive_default_bash_command() -> List[str]:
    bash = os.environ.get("BASH", "/bin/bash")
    return [bash, "--noprofile", "--norc", "{source}"]


def parse_command_template(raw: Optional[str], default: List[str]) -> List[str]:
    if raw is None:
        return list(default)
    return shlex.split(raw)


def load_manifest(path: Path) -> List[TestCase]:
    data = json.loads(path.read_text(encoding="utf-8"))
    tests: List[TestCase] = []
    base = path.parent
    for raw in data.get("tests", []):
        script_path = (base / raw["script"]).resolve()
        tests.append(
            TestCase(
                test_id=raw["id"],
                name=raw.get("name", raw["id"]),
                category=raw.get("category", "misc"),
                description=raw.get("description", ""),
                script=script_path,
                expect=raw.get("expect", "runtime_ok"),
                args=list(raw.get("args", [])),
                bash_args=list(raw.get("bash_args", [])),
                env=dict(raw.get("env", {})),
                stdin=raw.get("stdin"),
                expected_stdout=raw.get("expected_stdout"),
                expected_stderr_substring=raw.get("expected_stderr_substring"),
            )
        )
    return tests


def format_command(template: List[str], context: Dict[str, str]) -> List[str]:
    return [part.format(**context) for part in template]


def run_command(command: List[str], env: Dict[str, str], stdin: Optional[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        text=True,
        capture_output=True,
        timeout=DEFAULT_TIMEOUT,
        env=env,
        input=stdin,
    )


def _normalise_parity_stderr(text: str) -> str:
    lines = []
    for line in text.splitlines(keepends=True):
        if line.startswith(IGNORED_PARITY_STDERR_PREFIXES):
            continue
        lines.append(line)
    return "".join(lines)


def evaluate_case(
    case: TestCase,
    exsh_template: List[str],
    bash_template: List[str],
) -> TestResult:
    if not case.script.exists():
        return TestResult(
            case=case,
            passed=False,
            reason=f"script not found: {case.script}",
            exsh_stdout="",
            exsh_stderr="",
            exsh_returncode=-1,
        )

    base_env = os.environ.copy()
    base_env.update(case.env)

    context = {
        "source": str(case.script),
        "script": str(case.script),
        "repo": str(REPO_ROOT),
        "tests": str(case.script.parent),
        "script_basename": case.script.name,
    }

    exsh_command = format_command(exsh_template + case.args, context)
    exsh_proc = run_command(exsh_command, base_env, case.stdin)

    passed = True
    reason: Optional[str] = None

    if case.expect == "runtime_ok":
        if exsh_proc.returncode != 0:
            passed = False
            reason = f"unexpected return code {exsh_proc.returncode}"
    elif case.expect == "runtime_error":
        if exsh_proc.returncode == 0:
            passed = False
            reason = "expected non-zero return code"
    elif case.expect == "match_bash":
        bash_command = format_command(bash_template + case.bash_args, context)
        bash_proc = run_command(bash_command, base_env, case.stdin)
        exsh_stderr_norm = _normalise_parity_stderr(exsh_proc.stderr)
        bash_stderr_norm = _normalise_parity_stderr(bash_proc.stderr)
        if exsh_proc.returncode != bash_proc.returncode:
            passed = False
            reason = (
                f"exit mismatch (exsh={exsh_proc.returncode}, bash={bash_proc.returncode})"
            )
        elif exsh_proc.stdout != bash_proc.stdout:
            passed = False
            reason = "stdout differs from bash"
        elif exsh_stderr_norm != bash_stderr_norm:
            passed = False
            reason = "stderr differs from bash"
        if passed and case.expected_stdout is not None:
            if exsh_proc.stdout.strip() != case.expected_stdout.strip():
                passed = False
                reason = "stdout mismatch"
        if passed and case.expected_stderr_substring:
            if case.expected_stderr_substring not in exsh_proc.stderr:
                passed = False
                reason = "missing stderr substring"
        return TestResult(
            case=case,
            passed=passed,
            reason=reason,
            exsh_stdout=exsh_proc.stdout,
            exsh_stderr=exsh_proc.stderr,
            exsh_returncode=exsh_proc.returncode,
            bash_stdout=bash_proc.stdout,
            bash_stderr=bash_proc.stderr,
            bash_returncode=bash_proc.returncode,
        )
    else:
        passed = False
        reason = f"unknown expectation '{case.expect}'"

    if case.expected_stdout is not None and passed:
        if exsh_proc.stdout.strip() != case.expected_stdout.strip():
            passed = False
            reason = "stdout mismatch"
    if case.expected_stderr_substring and passed:
        if case.expected_stderr_substring not in exsh_proc.stderr:
            passed = False
            reason = "missing stderr substring"

    return TestResult(
        case=case,
        passed=passed,
        reason=reason,
        exsh_stdout=exsh_proc.stdout,
        exsh_stderr=exsh_proc.stderr,
        exsh_returncode=exsh_proc.returncode,
    )


def write_csv(out_dir: Path, results: List[TestResult]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "latest.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["id", "category", "expect", "pass", "reason", "exit", "bash_exit"])
        for result in results:
            writer.writerow(
                [
                    result.case.test_id,
                    result.case.category,
                    result.case.expect,
                    "pass" if result.passed else "fail",
                    result.reason or "",
                    result.exsh_returncode,
                    "" if result.case.expect != "match_bash" else result.bash_returncode,
                ]
            )


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run exsh scope verification tests")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Path to manifest.json")
    parser.add_argument("--only", type=str, default=None, help="Run only tests whose id contains this substring")
    parser.add_argument("--list", action="store_true", help="List available tests and exit")
    parser.add_argument("--cmd", type=str, default=None, help="Override exsh command template")
    parser.add_argument("--bash-cmd", type=str, default=None, help="Override bash command template for parity checks")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT_DIR, help="Directory for CSV summaries")
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    tests = load_manifest(args.manifest)
    if args.only:
        tests = [case for case in tests if args.only in case.test_id]
    if args.list:
        for case in tests:
            print(f"{case.test_id}: {case.name} ({case.category})")
            if case.description:
                print(f"  {case.description}")
        return 0

    if not tests:
        print("No tests selected", file=sys.stderr)
        return 1

    exsh_template = parse_command_template(args.cmd, derive_default_exsh_command())
    bash_template = parse_command_template(args.bash_cmd, derive_default_bash_command())

    results = [evaluate_case(case, exsh_template, bash_template) for case in tests]
    write_csv(args.out, results)

    failures = [result for result in results if not result.passed]
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        print(f"[{status}] {result.case.test_id} – {result.case.name}")
        if result.case.description:
            print(f"    {result.case.description}")
        if not result.passed:
            if result.reason:
                print(f"    Reason: {result.reason}")
            if result.exsh_stdout:
                print("    exsh stdout:")
                for line in result.exsh_stdout.rstrip().splitlines():
                    print(f"        {line}")
            if result.exsh_stderr:
                print("    exsh stderr:")
                for line in result.exsh_stderr.rstrip().splitlines():
                    print(f"        {line}")
            if result.case.expect == "match_bash" and result.bash_stdout is not None:
                if result.bash_stdout:
                    print("    bash stdout:")
                    for line in result.bash_stdout.rstrip().splitlines():
                        print(f"        {line}")
                if result.bash_stderr:
                    print("    bash stderr:")
                    for line in result.bash_stderr.rstrip().splitlines():
                        print(f"        {line}")

    print()
    print(f"Ran {len(results)} exsh scope test(s); {len(failures)} failure(s)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
