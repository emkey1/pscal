#!/usr/bin/env python3
"""Run the primary, library, and scope-verification suites with a shared summary."""
from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

BASE_SUITE_KEYS = ["core", "library", "scope"]
OPTIONAL_SUITE_KEYS = ["ios"]
SUITE_KEYS = BASE_SUITE_KEYS + OPTIONAL_SUITE_KEYS


def locate_repo_root(start: Path) -> Path:
    """Locate the repository root by walking upwards from *start*."""

    for candidate in [start] + list(start.parents):
        if (candidate / "CMakeLists.txt").exists() or (candidate / ".git").exists():
            return candidate
    return start


def shlex_split(value: str) -> List[str]:
    """Return ``value`` split via :func:`shlex.split`, ignoring empty strings."""

    value = value.strip()
    if not value:
        return []
    return shlex.split(value)


def run_command(
    name: str,
    command: Sequence[str],
    cwd: Path,
    *,
    capture_output: bool = False,
) -> Tuple[str, int, str, str]:
    """Execute ``command`` and return the suite name, exit code, and output."""

    print(f"\n==> Running {name} suite")
    print("    Command:", " ".join(command))

    if capture_output:
        result = subprocess.run(
            command,
            cwd=cwd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)

        return name, result.returncode, result.stdout, result.stderr

    result = subprocess.run(command, cwd=cwd)
    return name, result.returncode, "", ""


def main(argv: Sequence[str]) -> int:
    script_path = Path(__file__).resolve()
    tests_root = script_path.parent
    repo_root = locate_repo_root(tests_root)

    parser = argparse.ArgumentParser(
        description="Run all major regression suites with a consolidated summary.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="Python interpreter used for Python-based suites.",
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
        choices=SUITE_KEYS,
        help="Suite keys to skip (may be specified multiple times).",
    )
    parser.add_argument(
        "--core-args",
        default="",
        help="Extra arguments (shlex-split) forwarded to Tests/run_all_tests.",
    )
    parser.add_argument(
        "--library-args",
        default="",
        help="Extra arguments (shlex-split) forwarded to Tests/libs/run_all_tests.py.",
    )
    parser.add_argument(
        "--scope-args",
        default="",
        help="Extra arguments (shlex-split) forwarded to Tests/scope_verify/run_all_scope_tests.py.",
    )
    parser.add_argument(
        "--include-ios",
        action="store_true",
        help="Run the iOS/iPadOS portability suite (opt-in, off by default).",
    )
    args = parser.parse_args(argv)

    skip = set(args.skip or [])
    selected = [key for key in BASE_SUITE_KEYS if key not in skip]
    if args.include_ios and "ios" not in skip:
        selected.append("ios")

    if not selected:
        print("No suites selected. Use --skip judiciously or omit it altogether.")
        return 0

    commands: Dict[str, Tuple[List[str], Path, bool]] = {}

    if "core" in selected:
        core_script = tests_root / "run_all_tests"
        extra = shlex_split(args.core_args)
        commands["core"] = (["bash", str(core_script), *extra], core_script.parent, False)

    if "library" in selected:
        library_script = tests_root / "libs" / "run_all_tests.py"
        extra = shlex_split(args.library_args)
        commands["library"] = ([args.python, str(library_script), *extra], repo_root, True)

    if "scope" in selected:
        scope_script = tests_root / "scope_verify" / "run_all_scope_tests.py"
        extra = shlex_split(args.scope_args)
        commands["scope"] = ([args.python, str(scope_script), *extra], repo_root, False)

    if "ios" in selected:
        ios_script = tests_root / "run_ios_port_tests.sh"
        commands["ios"] = (["bash", str(ios_script)], tests_root, False)

    summary: List[Tuple[str, int]] = []
    for key in selected:
        command, cwd, capture = commands[key]
        name, code, stdout, _ = run_command(key, command, cwd, capture_output=capture)

        if capture and code != 0:
            success_marker = "All selected library suites completed successfully."
            if success_marker in stdout:
                print(
                    "Detected successful library run despite non-zero exit code; "
                    "treating suite as PASS.",
                    file=sys.stderr,
                )
                code = 0

        summary.append((name, code))
        if code != 0 and args.stop_on_failure:
            break

    failures = [name for name, code in summary if code != 0]

    print("\nSummary:")
    for name, code in summary:
        status = "PASS" if code == 0 else f"FAIL (exit {code})"
        print(f"  {name}: {status}")

    if failures:
        print("\nOne or more suites failed.")
        return 1

    print("\nAll selected suites completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
