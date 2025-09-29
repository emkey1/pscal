#!/usr/bin/env python3
"""Run all language scope verification harnesses.

This helper script walks the scope verification sub-packages (C-like, Pascal,
Rea) and runs each language-specific harness sequentially. Any additional
arguments provided after the script's own options are forwarded verbatim to
each harness invocation.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import List, Sequence, Tuple

# Map of language key -> (subdirectory name, harness filename)
LANGUAGE_HARNESSES = {
    "clike": ("clike", "clike_scope_test_harness.py"),
    "pascal": ("pascal", "pascal_scope_test_harness.py"),
    "rea": ("rea", "rea_scope_test_harness.py"),
}


def locate_repo_root(start: Path) -> Path:
    """Locate the repository root by walking upwards from *start*."""

    for candidate in [start] + list(start.parents):
        if (candidate / "CMakeLists.txt").exists() or (candidate / ".git").exists():
            return candidate
    return start


def build_harness_command(
    python: str,
    harness_path: Path,
    manifest_path: Path,
    extra_args: Sequence[str],
) -> List[str]:
    """Construct the command used to execute an individual harness."""

    base_cmd = [python, str(harness_path), "--manifest", str(manifest_path)]
    base_cmd.extend(extra_args)
    return base_cmd


def run_harness(
    name: str,
    python: str,
    harness_path: Path,
    manifest_path: Path,
    extra_args: Sequence[str],
    cwd: Path,
) -> Tuple[str, int]:
    """Execute a single harness and return its (name, returncode)."""

    cmd = build_harness_command(python, harness_path, manifest_path, extra_args)
    print(f"\n==> Running {name} scope tests")
    print("    Command:", " ".join(cmd))
    result = subprocess.run(cmd, cwd=cwd)
    return name, result.returncode


def main(argv: Sequence[str]) -> int:
    script_path = Path(__file__).resolve()
    scope_root = script_path.parent
    repo_root = locate_repo_root(scope_root)

    parser = argparse.ArgumentParser(
        description="Run every language scope verification harness sequentially.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="Python interpreter used to launch each harness.",
    )
    parser.add_argument(
        "--stop-on-failure",
        action="store_true",
        help="Abort after the first failing harness instead of running the rest.",
    )
    parser.add_argument(
        "--skip",
        action="append",
        default=[],
        choices=sorted(LANGUAGE_HARNESSES.keys()),
        help="Language keys to skip (can be specified multiple times).",
    )
    args, extra = parser.parse_known_args(argv)

    selected_languages = [
        key
        for key in sorted(LANGUAGE_HARNESSES.keys())
        if key not in set(args.skip or [])
    ]

    if not selected_languages:
        print("No harnesses selected. Use --skip judiciously or omit it altogether.")
        return 0

    summary: List[Tuple[str, int]] = []
    for key in selected_languages:
        subdir, filename = LANGUAGE_HARNESSES[key]
        harness_path = scope_root / subdir / filename
        manifest_path = harness_path.parent / "tests" / "manifest.json"
        if not harness_path.exists():
            print(f"Skipping {key}: missing harness at {harness_path}")
            summary.append((key, 127))
            if args.stop_on_failure:
                break
            continue
        if not manifest_path.exists():
            print(f"Skipping {key}: missing manifest at {manifest_path}")
            summary.append((key, 127))
            if args.stop_on_failure:
                break
            continue

        name, returncode = run_harness(
            key,
            args.python,
            harness_path,
            manifest_path,
            extra,
            repo_root,
        )
        summary.append((name, returncode))
        if returncode != 0 and args.stop_on_failure:
            break

    failures = [name for name, code in summary if code != 0]

    print("\nSummary:")
    for name, code in summary:
        status = "PASS" if code == 0 else f"FAIL (exit {code})"
        print(f"  {name}: {status}")

    if failures:
        print("\nOne or more harnesses failed.")
        return 1

    print("\nAll selected harnesses completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
