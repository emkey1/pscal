#!/usr/bin/env python3
"""Run every optional library test suite shipped under ``Tests/libs``.

This helper mirrors ``Tests/scope_verify/run_all_scope_tests.py`` but targets the
legacy library regression suites that previously lived under ``etc/tests``.  The
script locates the repository root, runs each language-specific harness, and
presents a concise summary of pass/fail results.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import List, Sequence, Tuple

LANGUAGE_SCRIPTS = {
    "clike": ("clike", "run_tests.py"),
    "pascal": ("pascal", "run_tests.py"),
    "rea": ("rea", "run_tests.py"),
}


def locate_repo_root(start: Path) -> Path:
    """Locate the repository root by walking upwards from *start*."""

    for candidate in [start] + list(start.parents):
        if (candidate / "CMakeLists.txt").exists() or (candidate / ".git").exists():
            return candidate
    return start


def build_command(python: str, script_path: Path, extra_args: Sequence[str]) -> List[str]:
    """Construct the command used to execute a language suite."""

    cmd = [python, str(script_path)]
    cmd.extend(extra_args)
    return cmd


def run_suite(
    name: str,
    python: str,
    script_path: Path,
    extra_args: Sequence[str],
    cwd: Path,
) -> Tuple[str, int]:
    """Execute a single suite and return its ``(name, returncode)`` tuple."""

    command = build_command(python, script_path, extra_args)
    print(f"\n==> Running {name} library tests")
    print("    Command:", " ".join(command))
    result = subprocess.run(command, cwd=cwd)
    return name, result.returncode


def main(argv: Sequence[str]) -> int:
    script_path = Path(__file__).resolve()
    libs_root = script_path.parent
    repo_root = locate_repo_root(libs_root)

    parser = argparse.ArgumentParser(
        description="Run all optional library regression suites.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="Python interpreter used to launch each suite.",
    )
    parser.add_argument(
        "--stop-on-failure",
        action="store_true",
        help="Abort after the first failing suite instead of running the rest.",
    )
    parser.add_argument(
        "--skip",
        action="append",
        default=[],
        choices=sorted(LANGUAGE_SCRIPTS.keys()),
        help="Language keys to skip (can be specified multiple times).",
    )
    args, extra = parser.parse_known_args(argv)

    selected = [
        key for key in sorted(LANGUAGE_SCRIPTS.keys()) if key not in set(args.skip or [])
    ]

    if not selected:
        print("No suites selected. Use --skip judiciously or omit it altogether.")
        return 0

    summary: List[Tuple[str, int]] = []
    for key in selected:
        subdir, filename = LANGUAGE_SCRIPTS[key]
        suite_path = libs_root / subdir / filename
        if not suite_path.exists():
            print(f"Skipping {key}: missing suite at {suite_path}")
            summary.append((key, 127))
            if args.stop_on_failure:
                break
            continue

        name, code = run_suite(key, args.python, suite_path, extra, repo_root)
        summary.append((name, code))
        if code != 0 and args.stop_on_failure:
            break

    failures = [name for name, code in summary if code != 0]

    print("\nSummary:")
    for name, code in summary:
        status = "PASS" if code == 0 else f"FAIL (exit {code})"
        print(f"  {name}: {status}")

    if failures:
        print("\nOne or more library suites failed.")
        return 1

    print("\nAll selected library suites completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
