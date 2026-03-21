#!/usr/bin/env python3
"""Smoke tests for SmallClue git path-truncate awareness."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence


def run_cmd(argv: Sequence[str], *, cwd: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(argv),
        cwd=str(cwd),
        env=env,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
    )


def ensure(cond: bool, message: str, proc: subprocess.CompletedProcess[str] | None = None) -> None:
    if cond:
        return
    if proc is not None:
        raise RuntimeError(
            f"{message}\n"
            f"returncode={proc.returncode}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    raise RuntimeError(message)


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Run SmallClue git path-truncate smoke tests.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--smallclue",
        default=str(Path(__file__).resolve().parents[2] / "build" / "bin" / "smallclue"),
        help="SmallClue executable path.",
    )
    args = parser.parse_args(argv)

    smallclue = Path(args.smallclue)
    if not smallclue.exists():
        print(f"smallclue executable not found: {smallclue}", file=sys.stderr)
        return 2

    # This smoke test targets PSCAL iOS/iPadOS behavior. On non-iOS builds
    # (where path virtualization wrappers are not compiled in), skip cleanly.
    probe = run_cmd([str(smallclue), "type", "addt"], cwd=Path.cwd(), env=os.environ.copy())
    if probe.returncode != 0:
        print("git-path-truncate smoke: skipped (non-iOS/non-iPadOS smallclue build)")
        return 0

    tests_total = 0
    tests_passed = 0
    repo_root = Path(__file__).resolve().parents[2]

    try:
        with tempfile.TemporaryDirectory(prefix="pscal_git_pathtr_") as tmp:
            prefix = Path(tmp) / "container-root"
            prefix.mkdir(parents=True, exist_ok=True)

            env = os.environ.copy()
            env["PATH_TRUNCATE"] = str(prefix)

            # Case 1: absolute virtual -C path is expanded for operations but
            # displayed as virtual/container-relative in output.
            tests_total += 1
            virtual_repo1 = "/home/git/project_abs"
            host_repo1 = prefix / virtual_repo1.lstrip("/")
            host_repo1.mkdir(parents=True, exist_ok=True)

            proc = run_cmd(
                [str(smallclue), "git", "-C", virtual_repo1, "init"],
                cwd=repo_root,
                env=env,
            )
            ensure(proc.returncode == 0, "git init with virtual -C failed", proc)
            ensure("/home/git/project_abs/.git" in proc.stdout, "git init did not print virtual repo path", proc)
            ensure(str(prefix) not in proc.stdout, "git init leaked host/container prefix", proc)

            proc = run_cmd(
                [str(smallclue), "git", "-C", virtual_repo1, "rev-parse", "--show-toplevel"],
                cwd=repo_root,
                env=env,
            )
            ensure(proc.returncode == 0, "rev-parse --show-toplevel with virtual -C failed", proc)
            ensure(proc.stdout.strip() == virtual_repo1, "show-toplevel was not virtualized", proc)
            tests_passed += 1

            # Case 2: no -C should still operate at cwd (host-expanded) while
            # output remains virtualized when PATH_TRUNCATE is active.
            tests_total += 1
            virtual_repo2 = "/home/git/project_cwd"
            host_repo2 = prefix / virtual_repo2.lstrip("/")
            host_repo2.mkdir(parents=True, exist_ok=True)

            proc = run_cmd([str(smallclue), "git", "init"], cwd=host_repo2, env=env)
            ensure(proc.returncode == 0, "git init from cwd failed", proc)
            ensure("/home/git/project_cwd/.git" in proc.stdout, "cwd git init did not print virtual path", proc)
            ensure(str(prefix) not in proc.stdout, "cwd git init leaked host/container prefix", proc)

            (host_repo2 / "hello.txt").write_text("hello\n", encoding="utf-8")
            proc = run_cmd(
                [str(smallclue), "git", "status", "--porcelain=v1"],
                cwd=host_repo2,
                env=env,
            )
            ensure(proc.returncode == 0, "git status from cwd failed", proc)
            ensure("?? hello.txt" in proc.stdout, "git status from cwd did not scope to repository cwd", proc)
            tests_passed += 1

    except Exception as exc:  # pragma: no cover - explicit failure path
        print(f"FAIL: {exc}", file=sys.stderr)
        print(f"git-path-truncate smoke: {tests_passed}/{tests_total} passed")
        return 1

    print(f"git-path-truncate smoke: {tests_passed}/{tests_total} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
