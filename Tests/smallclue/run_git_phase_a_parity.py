#!/usr/bin/env python3
"""Run SmallClue git Phase A parity tests from a manifest."""

from __future__ import annotations

import argparse
import difflib
import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple


@dataclass
class TestResult:
    test_id: str
    ok: bool
    reason: str
    details: str


def locate_repo_root(start: Path) -> Path:
    for candidate in [start] + list(start.parents):
        if (candidate / "CMakeLists.txt").exists() or (candidate / ".git").exists():
            return candidate
    return start


def run_cmd(argv: Sequence[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        list(argv),
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def parse_key_value_lines(text: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            out[key.strip()] = value.strip()
    return out


def apply_tokens(text: str, tokens: Dict[str, str]) -> str:
    rendered = text
    for key, value in tokens.items():
        rendered = rendered.replace("${" + key + "}", value)
    return rendered


def path_variants(path: str) -> List[str]:
    variants = {path, os.path.realpath(path)}
    extra = set()
    for candidate in list(variants):
        if candidate.startswith("/private/"):
            extra.add(candidate[len("/private") :])
        else:
            extra.add("/private" + candidate)
    variants |= extra
    return sorted(variants, key=len, reverse=True)


def normalize_fixture_paths(text: str, tokens: Dict[str, str]) -> str:
    repo_root = tokens.get("REPO_ROOT")
    if not repo_root:
        return text
    rendered = text
    for variant in path_variants(repo_root):
        rendered = rendered.replace(variant, repo_root)
    return rendered


def compare_text(expected: str, actual: str, mode: str) -> Tuple[bool, str]:
    if mode == "exact":
        if expected == actual:
            return True, ""
        diff = "\n".join(
            difflib.unified_diff(
                expected.splitlines(),
                actual.splitlines(),
                fromfile="expected",
                tofile="actual",
                lineterm="",
            )
        )
        return False, diff
    if mode == "contains":
        if expected in actual:
            return True, ""
        return False, f"expected substring not found: {expected!r}"
    return False, f"unsupported comparison mode: {mode}"


def resolve_manifest(path: Path) -> Dict[str, object]:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def filter_tests(tests: List[Dict[str, object]], only: str) -> List[Dict[str, object]]:
    if not only:
        return tests
    needle = only.lower()
    return [t for t in tests if needle in str(t.get("id", "")).lower()]


def run_case(
    test: Dict[str, object],
    *,
    repo_root: Path,
    tokens: Dict[str, str],
    git_bin: str,
    smallclue_bin: Path,
    baseline_only: bool,
    check_baseline: bool,
) -> TestResult:
    test_id = str(test.get("id", "<unknown>"))
    comparison = test.get("comparison", {}) or {}
    stdout_mode = str(comparison.get("stdout", "exact"))
    stderr_mode = str(comparison.get("stderr", "exact"))
    exit_mode = str(comparison.get("exit", "exact"))

    expected_exit = int(test.get("expected_exit", 0))
    expected_stdout = normalize_fixture_paths(
        apply_tokens(str(test.get("expected_stdout", "")), tokens), tokens
    )
    expected_stderr = normalize_fixture_paths(
        apply_tokens(str(test.get("expected_stderr", "")), tokens), tokens
    )

    git_argv = list(test.get("git_argv", []))
    if not git_argv:
        return TestResult(test_id, False, "bad-manifest", "missing git_argv")

    baseline = run_cmd([git_bin, *git_argv], repo_root)
    baseline_stdout = normalize_fixture_paths(baseline.stdout, tokens)
    baseline_stderr = normalize_fixture_paths(baseline.stderr, tokens)
    if check_baseline:
        if exit_mode == "exact" and baseline.returncode != expected_exit:
            return TestResult(
                test_id,
                False,
                "baseline-exit",
                f"expected={expected_exit} actual={baseline.returncode}",
            )
        ok, diff = compare_text(expected_stdout, baseline_stdout, stdout_mode)
        if not ok:
            return TestResult(test_id, False, "baseline-stdout", diff)
        ok, diff = compare_text(expected_stderr, baseline_stderr, stderr_mode)
        if not ok:
            return TestResult(test_id, False, "baseline-stderr", diff)

    if baseline_only:
        return TestResult(test_id, True, "ok", "")

    smallclue_argv = list(test.get("smallclue_argv", []))
    if not smallclue_argv:
        return TestResult(test_id, False, "bad-manifest", "missing smallclue_argv")

    actual = run_cmd([str(smallclue_bin), *smallclue_argv], repo_root)
    actual_stdout = normalize_fixture_paths(actual.stdout, tokens)
    actual_stderr = normalize_fixture_paths(actual.stderr, tokens)

    if exit_mode == "exact" and actual.returncode != expected_exit:
        return TestResult(
            test_id,
            False,
            "smallclue-exit",
            f"expected={expected_exit} actual={actual.returncode}",
        )

    ok, diff = compare_text(expected_stdout, actual_stdout, stdout_mode)
    if not ok:
        return TestResult(test_id, False, "smallclue-stdout", diff)

    ok, diff = compare_text(expected_stderr, actual_stderr, stderr_mode)
    if not ok:
        return TestResult(test_id, False, "smallclue-stderr", diff)

    return TestResult(test_id, True, "ok", "")


def main(argv: Sequence[str]) -> int:
    script_path = Path(__file__).resolve()
    tests_root = script_path.parents[1]
    repo_root = locate_repo_root(tests_root)

    parser = argparse.ArgumentParser(
        description="Run SmallClue git Phase A parity tests.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--manifest",
        default=str(tests_root / "smallclue" / "git_phase_a" / "manifest.json"),
        help="Path to parity manifest.",
    )
    parser.add_argument(
        "--smallclue",
        default=str(repo_root / "build" / "bin" / "smallclue"),
        help="SmallClue executable path.",
    )
    parser.add_argument(
        "--git-bin",
        default="git",
        help="System git executable used for baseline checks.",
    )
    parser.add_argument(
        "--only",
        default="",
        help="Run only tests whose id contains this substring (case-insensitive).",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List test IDs and exit.",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="Stop at first failing case.",
    )
    parser.add_argument(
        "--baseline-only",
        action="store_true",
        help="Validate manifest expectations against system git only.",
    )
    parser.add_argument(
        "--skip-baseline-check",
        action="store_true",
        help="Skip baseline verification against system git expectations.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print command-level progress.",
    )
    args = parser.parse_args(argv)

    manifest_path = Path(args.manifest)
    manifest = resolve_manifest(manifest_path)
    tests = manifest.get("tests", [])
    if not isinstance(tests, list):
        print("manifest error: 'tests' must be a list", file=sys.stderr)
        return 2

    selected = filter_tests(tests, args.only)
    if args.list:
        for case in selected:
            print(case.get("id", "<unknown>"))
        return 0

    fixture = manifest.get("fixture", {})
    if not isinstance(fixture, dict):
        print("manifest error: 'fixture' must be an object", file=sys.stderr)
        return 2
    setup_script_rel = fixture.get("setup_script")
    if not setup_script_rel:
        print("manifest error: missing fixture.setup_script", file=sys.stderr)
        return 2

    setup_script = repo_root / str(setup_script_rel)
    if not setup_script.exists():
        print(f"fixture setup script missing: {setup_script}", file=sys.stderr)
        return 2

    setup_proc = run_cmd([str(setup_script)], repo_root)
    if setup_proc.returncode != 0:
        print("fixture setup failed", file=sys.stderr)
        if setup_proc.stdout:
            print(setup_proc.stdout, end="", file=sys.stderr)
        if setup_proc.stderr:
            print(setup_proc.stderr, end="", file=sys.stderr)
        return 2

    tokens = parse_key_value_lines(setup_proc.stdout)
    fixture_repo = tokens.get("REPO_ROOT")
    if not fixture_repo:
        print("fixture setup output missing REPO_ROOT", file=sys.stderr)
        return 2
    fixture_repo_path = Path(fixture_repo)
    if not fixture_repo_path.exists():
        print(f"fixture repo does not exist: {fixture_repo_path}", file=sys.stderr)
        return 2

    smallclue_bin = Path(args.smallclue)
    if not args.baseline_only and not smallclue_bin.exists():
        print(
            f"smallclue executable not found: {smallclue_bin}\n"
            "Build it first or run with --baseline-only.",
            file=sys.stderr,
        )
        return 2

    total = len(selected)
    passed = 0
    failed: List[TestResult] = []

    for index, case in enumerate(selected, start=1):
        test_id = str(case.get("id", "<unknown>"))
        if args.verbose:
            print(f"[{index}/{total}] {test_id}")
        result = run_case(
            case,
            repo_root=fixture_repo_path,
            tokens=tokens,
            git_bin=args.git_bin,
            smallclue_bin=smallclue_bin,
            baseline_only=args.baseline_only,
            check_baseline=not args.skip_baseline_check,
        )
        if result.ok:
            passed += 1
            continue

        failed.append(result)
        print(f"FAIL {result.test_id}: {result.reason}", file=sys.stderr)
        if result.details:
            print(result.details, file=sys.stderr)
        if args.fail_fast:
            break

    print(f"git-phase-a parity: {passed}/{total} passed")
    if failed:
        print(f"{len(failed)} test(s) failed.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
