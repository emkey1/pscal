#!/usr/bin/env python3
"""Manifest-driven regression harness for the PSCAL exsh front end."""

from __future__ import annotations

import argparse
import json
import os
import shlex
from pathlib import Path
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional

HARNESS_ROOT = Path(__file__).resolve().parent
REPO_ROOT = HARNESS_ROOT.parent.parent
DEFAULT_MANIFEST = HARNESS_ROOT / "tests" / "manifest.json"
DEFAULT_TIMEOUT = 20.0
IGNORED_PARITY_STDERR_PREFIXES = (
    "Compilation successful.",
    "Loaded cached bytecode",
    "Loaded cached bytecode",
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
    env: Dict[str, str]
    stdin: Optional[str]
    expected_stdout: Optional[str]
    expected_stderr_substring: Optional[str]
    prime_cache: bool
    bash_args: List[str]
    bash_cmd: Optional[str]


@dataclass
class TestResult:
    case: TestCase
    passed: bool
    reason: Optional[str] = None
    stdout: str = ""
    stderr: str = ""
    returncode: Optional[int] = None
    bash_stdout: Optional[str] = None
    bash_stderr: Optional[str] = None
    bash_returncode: Optional[int] = None


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
                stdin=raw.get("stdin"),
                expected_stdout=raw.get("expected_stdout"),
                expected_stderr_substring=raw.get("expected_stderr_substring"),
                prime_cache=bool(raw.get("prime_cache", False)),
                bash_args=list(raw.get("bash_args", [])),
                bash_cmd=raw.get("bash_cmd"),
            )
        )
    return tests


def ensure_executable(path_override: Optional[str]) -> Path:
    if path_override:
        exe = Path(path_override).expanduser()
    else:
        exe = REPO_ROOT / "build" / "bin" / "exsh"
    if not exe.exists():
        raise FileNotFoundError(f"exsh executable not found at {exe}; build the project first or pass --executable")
    return exe


def ensure_bash(command_override: Optional[str]) -> Path:
    if command_override:
        candidate = Path(shlex.split(command_override)[0])
        if candidate.exists():
            return candidate
        raise FileNotFoundError(f"bash override not found: {candidate}")
    candidates: List[Path] = []
    env_bash = os.environ.get("BASH")
    if env_bash:
        candidates.append(Path(env_bash))
    candidates.extend(
        [
            Path("/opt/homebrew/bin/bash"),
            Path("/usr/local/bin/bash"),
            Path("/bin/bash"),
        ]
    )
    for candidate in candidates:
        if candidate.exists():
            os.environ["BASH"] = str(candidate)
            return candidate
    raise FileNotFoundError("Unable to locate a bash executable")


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
        input=case.stdin,
    )


def run_bash(
    bash_executable: Path,
    case: TestCase,
    override_cmd: Optional[str],
    extra_args: List[str],
    stdin: Optional[str],
) -> subprocess.CompletedProcess[str]:
    if override_cmd:
        cmd = shlex.split(override_cmd)
        cmd = [part.format(script=str(case.script)) for part in cmd]
    else:
        cmd = [str(bash_executable), "--noprofile", "--norc", *extra_args, str(case.script)]
    env = os.environ.copy()
    env.update(case.env)
    return subprocess.run(
        cmd,
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


def evaluate(
    case: TestCase,
    exsh_proc: subprocess.CompletedProcess[str],
    bash_proc: Optional[subprocess.CompletedProcess[str]] = None,
) -> TestResult:
    stdout_text = exsh_proc.stdout
    stderr_text = exsh_proc.stderr
    reason: Optional[str] = None
    passed = True

    # Allow tests to self-skip (return 77) without failing the suite.
    if exsh_proc.returncode == 77:
        return TestResult(
            case=case,
            passed=True,
            reason="skipped",
            stdout=stdout_text,
            stderr=stderr_text,
            returncode=exsh_proc.returncode,
            bash_stdout=bash_proc.stdout if bash_proc else None,
            bash_stderr=bash_proc.stderr if bash_proc else None,
            bash_returncode=bash_proc.returncode if bash_proc else None,
        )

    if case.expect == "runtime_ok":
        if exsh_proc.returncode != 0:
            passed = False
            reason = f"unexpected return code {exsh_proc.returncode}"
    elif case.expect in {"runtime_error", "parse_error"}:
        if exsh_proc.returncode == 0:
            passed = False
            reason = "expected non-zero return code"
    elif case.expect == "match_bash":
        if bash_proc is None:
            passed = False
            reason = "bash result missing"
        else:
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
        # fall through to optional explicit expectations
    else:
        passed = False
        reason = f"unknown expectation '{case.expect}'"

    if passed and case.expected_stdout is not None:
        if stdout_text.strip() != case.expected_stdout.strip():
            passed = False
            reason = "stdout mismatch"
    if passed and case.expected_stderr_substring:
        if case.expected_stderr_substring not in stderr_text:
            passed = False
            reason = "missing stderr substring"

    bash_stdout = bash_proc.stdout if bash_proc else None
    bash_stderr = bash_proc.stderr if bash_proc else None
    bash_returncode = bash_proc.returncode if bash_proc else None

    return TestResult(
        case=case,
        passed=passed,
        reason=reason,
        stdout=stdout_text,
        stderr=stderr_text,
        returncode=exsh_proc.returncode,
        bash_stdout=bash_stdout,
        bash_stderr=bash_stderr,
        bash_returncode=bash_returncode,
    )


def run_case(executable: Path, bash_executable: Path, case: TestCase) -> TestResult:
    if not case.script.exists():
        return TestResult(case=case, passed=False, reason=f"script not found: {case.script}")

    if case.prime_cache:
        try:
            run_exsh(executable, case)
        except subprocess.SubprocessError:
            pass  # ignore priming errors; actual run will report details

    proc = run_exsh(executable, case)
    bash_proc = None
    if case.expect == "match_bash":
        bash_override = case.bash_cmd
        bash_proc = run_bash(bash_executable, case, bash_override, case.bash_args, case.stdin)
    return evaluate(case, proc, bash_proc)


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run exsh front-end regression tests")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Path to test manifest")
    parser.add_argument("--only", type=str, default=None, help="Run only tests whose id contains this substring")
    parser.add_argument("--list", action="store_true", help="List available tests and exit")
    parser.add_argument("--executable", type=str, default=None, help="Override path to exsh executable")
    parser.add_argument("--bash-cmd", type=str, default=None, help="Override bash command used for match_bash expectations")
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
        executable = ensure_executable(args.executable)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    try:
        bash_executable = ensure_bash(args.bash_cmd)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    results = [run_case(executable, bash_executable, case) for case in tests]

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
            if result.case.expect == "match_bash" and result.bash_stdout is not None:
                if result.bash_stdout:
                    print("    bash stdout:\n" + "\n".join(f"        {line}" for line in result.bash_stdout.strip().splitlines()))
                if result.bash_stderr:
                    print("    bash stderr:\n" + "\n".join(f"        {line}" for line in result.bash_stderr.strip().splitlines()))

    print()
    print(f"Ran {len(results)} exsh test(s); {len(failures)} failure(s)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
