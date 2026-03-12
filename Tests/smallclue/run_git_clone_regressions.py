#!/usr/bin/env python3
"""Targeted regression checks for SmallClue git clone behavior."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


@dataclass
class CmdResult:
    returncode: int
    stdout: str
    stderr: str


def run_cmd(argv: Sequence[str], *, cwd: Path, env: dict[str, str] | None = None) -> CmdResult:
    proc = subprocess.run(
        list(argv),
        cwd=str(cwd),
        env=env,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
    )
    return CmdResult(proc.returncode, proc.stdout, proc.stderr)


def ensure_ok(result: CmdResult, context: str) -> None:
    if result.returncode == 0:
        return
    raise RuntimeError(
        f"{context} failed (rc={result.returncode})\n"
        f"stdout:\n{result.stdout}\n"
        f"stderr:\n{result.stderr}"
    )


def build_env() -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("GIT_AUTHOR_NAME", "PSCAL Tester")
    env.setdefault("GIT_AUTHOR_EMAIL", "pscal@example.com")
    env.setdefault("GIT_COMMITTER_NAME", "PSCAL Tester")
    env.setdefault("GIT_COMMITTER_EMAIL", "pscal@example.com")
    env.setdefault("GIT_AUTHOR_DATE", "2024-01-01T00:00:00Z")
    env.setdefault("GIT_COMMITTER_DATE", "2024-01-01T00:00:00Z")
    env.setdefault("GIT_CONFIG_NOSYSTEM", "1")
    return env


def first_stderr_line(text: str) -> str:
    for line in text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def case_clone_scp_implicit_dest(smallclue: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="pscal_git_clone_dest_") as td:
        root = Path(td).resolve()
        result = run_cmd([str(smallclue), "git", "clone", "user@host:repo.git"], cwd=root, env=build_env())
        if result.returncode == 0:
            raise RuntimeError("clone unexpectedly succeeded for synthetic SCP-style source")
        expected = f"Cloning into '{root / 'repo'}'..."
        actual = first_stderr_line(result.stderr)
        if actual != expected:
            raise RuntimeError(f"implicit clone destination mismatch\nexpected: {expected}\nactual:   {actual}")


def init_repo(path: Path, git_bin: str, env: dict[str, str]) -> None:
    path.mkdir(parents=True, exist_ok=True)
    ensure_ok(run_cmd([git_bin, "init", "-q"], cwd=path, env=env), f"init {path.name}")


def commit_file(repo: Path, git_bin: str, env: dict[str, str], rel_path: str, text: str, message: str) -> None:
    target = repo / rel_path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")
    ensure_ok(run_cmd([git_bin, "add", rel_path], cwd=repo, env=env), f"add {rel_path}")
    ensure_ok(run_cmd([git_bin, "commit", "-q", "-m", message], cwd=repo, env=env), f"commit {message}")


def case_clone_recurse_submodules_pathspec(smallclue: Path, git_bin: str) -> None:
    env = build_env()
    with tempfile.TemporaryDirectory(prefix="pscal_git_clone_submodules_") as td:
        root = Path(td)
        sub1 = root / "sub1"
        sub2 = root / "sub2"
        super_repo = root / "super"

        init_repo(sub1, git_bin, env)
        commit_file(sub1, git_bin, env, "file1.txt", "one\n", "sub1")

        init_repo(sub2, git_bin, env)
        commit_file(sub2, git_bin, env, "file2.txt", "two\n", "sub2")

        init_repo(super_repo, git_bin, env)
        commit_file(super_repo, git_bin, env, "root.txt", "root\n", "root")

        ensure_ok(
            run_cmd(
                [git_bin, "-c", "protocol.file.allow=always", "submodule", "add", "-q", "../sub1", "deps/sub1"],
                cwd=super_repo,
                env=env,
            ),
            "add submodule deps/sub1",
        )
        ensure_ok(
            run_cmd(
                [git_bin, "-c", "protocol.file.allow=always", "submodule", "add", "-q", "../sub2", "deps/sub2"],
                cwd=super_repo,
                env=env,
            ),
            "add submodule deps/sub2",
        )
        ensure_ok(run_cmd([git_bin, "commit", "-q", "-am", "add submodules"], cwd=super_repo, env=env), "commit submodules")

        cloned = root / "cloned"
        result = run_cmd(
            [str(smallclue), "git", "clone", "--recurse-submodules=deps/sub1", str(super_repo), str(cloned)],
            cwd=root,
            env=env,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"pathspec-limited recurse clone failed (rc={result.returncode})\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )
        if not (cloned / "deps" / "sub1" / "file1.txt").is_file():
            raise RuntimeError("selected submodule deps/sub1 was not cloned")
        if (cloned / "deps" / "sub2" / "file2.txt").exists():
            raise RuntimeError("unselected submodule deps/sub2 was cloned despite recurse pathspec")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--smallclue",
        default=str(Path(__file__).resolve().parents[2] / "build" / "bin" / "smallclue"),
        help="Path to smallclue executable",
    )
    parser.add_argument("--git-bin", default="git", help="System git executable")
    args = parser.parse_args()

    smallclue = Path(args.smallclue)
    if not smallclue.exists():
        print(f"smallclue executable not found: {smallclue}", file=sys.stderr)
        return 2

    cases = [
        ("clone_scp_implicit_dest", lambda: case_clone_scp_implicit_dest(smallclue)),
        ("clone_recurse_submodules_pathspec", lambda: case_clone_recurse_submodules_pathspec(smallclue, args.git_bin)),
    ]

    passed = 0
    for name, fn in cases:
        try:
            fn()
        except Exception as exc:  # pragma: no cover - failure reporting path
            print(f"[FAIL] {name}: {exc}", file=sys.stderr)
            return 1
        print(f"[PASS] {name}")
        passed += 1

    print(f"git-clone regressions: {passed}/{len(cases)} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
