#!/usr/bin/env python3
"""Run isolated parity checks for mutating SmallClue git commands."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple


@dataclass
class CaseResult:
    case_id: str
    ok: bool
    reason: str
    details: str


def run_cmd(argv: Sequence[str], cwd: Path, env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        list(argv),
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def ensure_ok(proc: subprocess.CompletedProcess, context: str) -> None:
    if proc.returncode != 0:
        raise RuntimeError(
            f"{context} failed (exit {proc.returncode})\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


def create_standard_repo(root: Path, git_bin: str) -> Path:
    repo = root / "repo"
    repo.mkdir(parents=True, exist_ok=True)
    ensure_ok(run_cmd([git_bin, "init", "-b", "main"], repo), "git init fixture")
    ensure_ok(run_cmd([git_bin, "config", "user.name", "PSCAL Tester"], repo), "git config user.name")
    ensure_ok(run_cmd([git_bin, "config", "user.email", "pscal@example.com"], repo), "git config user.email")

    (repo / "tracked.txt").write_text("base\n", encoding="utf-8")
    (repo / "keep.txt").write_text("keep\n", encoding="utf-8")
    env1 = dict(os.environ)
    env1.update({"GIT_AUTHOR_DATE": "2024-01-01T00:00:00Z", "GIT_COMMITTER_DATE": "2024-01-01T00:00:00Z"})
    ensure_ok(run_cmd([git_bin, "add", "tracked.txt", "keep.txt"], repo, env=env1), "git add fixture commit1")
    ensure_ok(run_cmd([git_bin, "commit", "-m", "initial"], repo, env=env1), "git commit fixture commit1")

    with (repo / "tracked.txt").open("a", encoding="utf-8") as fh:
        fh.write("second\n")
    env2 = dict(os.environ)
    env2.update({"GIT_AUTHOR_DATE": "2024-01-02T00:00:00Z", "GIT_COMMITTER_DATE": "2024-01-02T00:00:00Z"})
    ensure_ok(run_cmd([git_bin, "add", "tracked.txt"], repo, env=env2), "git add fixture commit2")
    ensure_ok(run_cmd([git_bin, "commit", "-m", "second"], repo, env=env2), "git commit fixture commit2")
    return repo


def apply_actions(repo: Path, actions: List[Dict[str, object]], git_bin: str) -> None:
    for action in actions:
        op = str(action.get("op", "")).strip()
        rel_path = action.get("path")
        if op == "write":
            if not isinstance(rel_path, str):
                raise RuntimeError("write action requires 'path'")
            text = str(action.get("text", ""))
            path = repo / rel_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            continue
        if op == "append":
            if not isinstance(rel_path, str):
                raise RuntimeError("append action requires 'path'")
            text = str(action.get("text", ""))
            path = repo / rel_path
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("a", encoding="utf-8") as fh:
                fh.write(text)
            continue
        if op == "remove":
            if not isinstance(rel_path, str):
                raise RuntimeError("remove action requires 'path'")
            path = repo / rel_path
            if path.exists():
                path.unlink()
            continue
        if op == "git":
            argv = action.get("argv")
            if not isinstance(argv, list) or not argv:
                raise RuntimeError("git action requires non-empty 'argv' list")
            proc = run_cmd([git_bin, *[str(a) for a in argv]], repo)
            ensure_ok(proc, f"pre-action git {' '.join(str(a) for a in argv)}")
            continue
        raise RuntimeError(f"unsupported action op: {op}")


def compare_streams(expected: str, actual: str) -> Tuple[bool, str]:
    if expected == actual:
        return True, ""
    return False, f"expected:\n{expected}\nactual:\n{actual}"


def run_case(case: Dict[str, object], *, git_bin: str, smallclue: Path) -> CaseResult:
    case_id = str(case.get("id", "<unknown>"))
    mode = str(case.get("mode", "repo"))
    git_argv = [str(x) for x in case.get("git_argv", [])]
    sc_argv = [str(x) for x in case.get("smallclue_argv", [])]
    actions = case.get("actions", [])
    checks = case.get("checks", [])
    if not git_argv or not sc_argv:
        return CaseResult(case_id, False, "bad-case", "missing git_argv or smallclue_argv")
    if not isinstance(actions, list) or not isinstance(checks, list):
        return CaseResult(case_id, False, "bad-case", "actions/checks must be lists")

    with tempfile.TemporaryDirectory(prefix="pscal_git_mut_") as tmp:
        tmp_path = Path(tmp)
        baseline_root = tmp_path / "baseline"
        actual_root = tmp_path / "actual"
        baseline_root.mkdir()
        actual_root.mkdir()

        baseline_repo = baseline_root
        actual_repo = actual_root
        if mode == "repo":
            baseline_repo = create_standard_repo(baseline_root, git_bin)
            actual_repo = create_standard_repo(actual_root, git_bin)
        elif mode == "root":
            pass
        else:
            return CaseResult(case_id, False, "bad-case", f"unsupported mode {mode!r}")

        apply_actions(baseline_repo, actions, git_bin)
        apply_actions(actual_repo, actions, git_bin)

        proc_git = run_cmd([git_bin, *git_argv], baseline_repo)
        proc_sc = run_cmd([str(smallclue), *sc_argv], actual_repo)

        if proc_git.returncode != proc_sc.returncode:
            return CaseResult(
                case_id,
                False,
                "exit",
                f"git={proc_git.returncode} smallclue={proc_sc.returncode}",
            )

        ok, detail = compare_streams(proc_git.stdout, proc_sc.stdout)
        if not ok:
            return CaseResult(case_id, False, "stdout", detail)
        ok, detail = compare_streams(proc_git.stderr, proc_sc.stderr)
        if not ok:
            return CaseResult(case_id, False, "stderr", detail)

        for check in checks:
            if not isinstance(check, dict):
                return CaseResult(case_id, False, "bad-case", "check entries must be objects")
            check_argv = check.get("git_argv")
            if not isinstance(check_argv, list) or not check_argv:
                return CaseResult(case_id, False, "bad-case", "check.git_argv must be a non-empty list")
            rel = str(check.get("repo_rel", "."))
            cwd_git = (baseline_repo / rel).resolve()
            cwd_sc = (actual_repo / rel).resolve()
            chk_git = run_cmd([git_bin, *[str(a) for a in check_argv]], cwd_git)
            chk_sc = run_cmd([git_bin, *[str(a) for a in check_argv]], cwd_sc)
            if chk_git.returncode != chk_sc.returncode:
                return CaseResult(
                    case_id,
                    False,
                    "check-exit",
                    f"{check_argv}: git={chk_git.returncode} smallclue-state={chk_sc.returncode}",
                )
            ok, detail = compare_streams(chk_git.stdout, chk_sc.stdout)
            if not ok:
                return CaseResult(case_id, False, "check-stdout", f"{check_argv}\n{detail}")
            ok, detail = compare_streams(chk_git.stderr, chk_sc.stderr)
            if not ok:
                return CaseResult(case_id, False, "check-stderr", f"{check_argv}\n{detail}")

    return CaseResult(case_id, True, "ok", "")


def build_cases() -> List[Dict[str, object]]:
    return [
        {
            "id": "init_basic_main_branch",
            "mode": "root",
            "git_argv": ["init", "-q", "-b", "main", "sandbox"],
            "smallclue_argv": ["git", "init", "-q", "-b", "main", "sandbox"],
            "actions": [],
            "checks": [
                {"repo_rel": "sandbox", "git_argv": ["rev-parse", "--abbrev-ref", "HEAD"]},
                {"repo_rel": "sandbox", "git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "add_single_file",
            "mode": "repo",
            "git_argv": ["add", "new.txt"],
            "smallclue_argv": ["git", "add", "new.txt"],
            "actions": [{"op": "write", "path": "new.txt", "text": "new file\n"}],
            "checks": [
                {"git_argv": ["diff", "--cached", "--name-status"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "add_all_stages_deletion_and_addition",
            "mode": "repo",
            "git_argv": ["add", "-A"],
            "smallclue_argv": ["git", "add", "-A"],
            "actions": [
                {"op": "write", "path": "untracked.txt", "text": "u\n"},
                {"op": "remove", "path": "keep.txt"},
            ],
            "checks": [
                {"git_argv": ["diff", "--cached", "--name-status"]},
            ],
        },
        {
            "id": "commit_quiet_staged_change",
            "mode": "repo",
            "git_argv": ["commit", "-q", "-m", "third change"],
            "smallclue_argv": ["git", "commit", "-q", "-m", "third change"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "third\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_all_tracked_with_a",
            "mode": "repo",
            "git_argv": ["commit", "-q", "-a", "-m", "commit all tracked"],
            "smallclue_argv": ["git", "commit", "-q", "-a", "-m", "commit all tracked"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "fourth\n"},
                {"op": "remove", "path": "keep.txt"},
                {"op": "write", "path": "untracked.txt", "text": "u\n"},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["show", "--name-status", "--pretty=", "HEAD"]},
            ],
        },
        {
            "id": "commit_allow_empty",
            "mode": "repo",
            "git_argv": ["commit", "-q", "--allow-empty", "-m", "empty commit"],
            "smallclue_argv": ["git", "commit", "-q", "--allow-empty", "-m", "empty commit"],
            "actions": [],
            "checks": [
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
            ],
        },
        {
            "id": "reset_hard_head_parent",
            "mode": "repo",
            "git_argv": ["reset", "--hard", "-q", "HEAD~1"],
            "smallclue_argv": ["git", "reset", "--hard", "-q", "HEAD~1"],
            "actions": [],
            "checks": [
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "restore_worktree_only",
            "mode": "repo",
            "git_argv": ["restore", "tracked.txt"],
            "smallclue_argv": ["git", "restore", "tracked.txt"],
            "actions": [{"op": "append", "path": "tracked.txt", "text": "dirty\n"}],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "restore_staged_only",
            "mode": "repo",
            "git_argv": ["restore", "--staged", "tracked.txt"],
            "smallclue_argv": ["git", "restore", "--staged", "tracked.txt"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "dirty\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "restore_staged_and_worktree",
            "mode": "repo",
            "git_argv": ["restore", "--staged", "--worktree", "tracked.txt"],
            "smallclue_argv": ["git", "restore", "--staged", "--worktree", "tracked.txt"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "dirty\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_create_from_head",
            "mode": "repo",
            "git_argv": ["branch", "topic-a"],
            "smallclue_argv": ["git", "branch", "topic-a"],
            "actions": [],
            "checks": [
                {"git_argv": ["show-ref", "--verify", "refs/heads/topic-a"]},
            ],
        },
        {
            "id": "branch_rename_existing",
            "mode": "repo",
            "git_argv": ["branch", "-m", "rename-src", "rename-dst"],
            "smallclue_argv": ["git", "branch", "-m", "rename-src", "rename-dst"],
            "actions": [
                {"op": "git", "argv": ["branch", "rename-src"]},
            ],
            "checks": [
                {"git_argv": ["show-ref", "--verify", "refs/heads/rename-dst"]},
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/heads/rename-src"]},
            ],
        },
        {
            "id": "branch_delete_merged",
            "mode": "repo",
            "git_argv": ["branch", "-d", "doomed"],
            "smallclue_argv": ["git", "branch", "-d", "doomed"],
            "actions": [
                {"op": "git", "argv": ["branch", "doomed"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/heads/doomed"]},
            ],
        },
        {
            "id": "tag_create_lightweight",
            "mode": "repo",
            "git_argv": ["tag", "light-v1"],
            "smallclue_argv": ["git", "tag", "light-v1"],
            "actions": [],
            "checks": [
                {"git_argv": ["show-ref", "--verify", "refs/tags/light-v1"]},
            ],
        },
        {
            "id": "tag_create_annotated",
            "mode": "repo",
            "git_argv": ["tag", "-a", "-m", "release-ann", "ann-v1"],
            "smallclue_argv": ["git", "tag", "-a", "-m", "release-ann", "ann-v1"],
            "actions": [],
            "checks": [
                {"git_argv": ["tag", "-n1", "ann-v1"]},
            ],
        },
        {
            "id": "tag_delete_existing",
            "mode": "repo",
            "git_argv": ["tag", "-d", "gone-v1"],
            "smallclue_argv": ["git", "tag", "-d", "gone-v1"],
            "actions": [
                {"op": "git", "argv": ["tag", "gone-v1"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/tags/gone-v1"]},
            ],
        },
        {
            "id": "checkout_existing_branch_quiet",
            "mode": "repo",
            "git_argv": ["checkout", "-q", "topic-checkout"],
            "smallclue_argv": ["git", "checkout", "-q", "topic-checkout"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-checkout"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--abbrev-ref", "HEAD"]},
            ],
        },
        {
            "id": "switch_create_branch_quiet",
            "mode": "repo",
            "git_argv": ["switch", "-q", "-c", "topic-switch"],
            "smallclue_argv": ["git", "switch", "-q", "-c", "topic-switch"],
            "actions": [],
            "checks": [
                {"git_argv": ["rev-parse", "--abbrev-ref", "HEAD"]},
            ],
        },
        {
            "id": "checkout_detached_head_quiet",
            "mode": "repo",
            "git_argv": ["checkout", "-q", "HEAD~1"],
            "smallclue_argv": ["git", "checkout", "-q", "HEAD~1"],
            "actions": [],
            "checks": [
                {"git_argv": ["symbolic-ref", "--quiet", "--short", "HEAD"]},
            ],
        },
        {
            "id": "config_set_key_value",
            "mode": "repo",
            "git_argv": ["config", "demo.value", "alpha"],
            "smallclue_argv": ["git", "config", "demo.value", "alpha"],
            "actions": [],
            "checks": [
                {"git_argv": ["config", "--get", "demo.value"]},
            ],
        },
        {
            "id": "config_add_and_get_all",
            "mode": "repo",
            "git_argv": ["config", "--add", "demo.multi", "two"],
            "smallclue_argv": ["git", "config", "--add", "demo.multi", "two"],
            "actions": [
                {"op": "git", "argv": ["config", "--add", "demo.multi", "one"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get-all", "demo.multi"]},
            ],
        },
        {
            "id": "config_replace_all_with_regex",
            "mode": "repo",
            "git_argv": ["config", "--replace-all", "demo.replace", "updated", "^t"],
            "smallclue_argv": ["git", "config", "--replace-all", "demo.replace", "updated", "^t"],
            "actions": [
                {"op": "git", "argv": ["config", "--add", "demo.replace", "one"]},
                {"op": "git", "argv": ["config", "--add", "demo.replace", "two"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get-all", "demo.replace"]},
            ],
        },
        {
            "id": "config_unset_single_key",
            "mode": "repo",
            "git_argv": ["config", "--unset", "demo.single"],
            "smallclue_argv": ["git", "config", "--unset", "demo.single"],
            "actions": [
                {"op": "git", "argv": ["config", "demo.single", "one"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get", "demo.single"]},
            ],
        },
        {
            "id": "config_unset_all_with_regex",
            "mode": "repo",
            "git_argv": ["config", "--unset-all", "demo.multi", "^t"],
            "smallclue_argv": ["git", "config", "--unset-all", "demo.multi", "^t"],
            "actions": [
                {"op": "git", "argv": ["config", "--add", "demo.multi", "one"]},
                {"op": "git", "argv": ["config", "--add", "demo.multi", "two"]},
                {"op": "git", "argv": ["config", "--add", "demo.multi", "three"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get-all", "demo.multi"]},
            ],
        },
    ]


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Run mutating git command parity between system git and SmallClue.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--git-bin", default="git", help="System git executable.")
    parser.add_argument(
        "--smallclue",
        default=str(Path(__file__).resolve().parents[2] / "build" / "bin" / "smallclue"),
        help="SmallClue executable path.",
    )
    parser.add_argument("--only", default="", help="Run only test IDs containing this substring.")
    parser.add_argument("--list", action="store_true", help="List test IDs and exit.")
    args = parser.parse_args(argv)

    cases = build_cases()
    if args.only:
        needle = args.only.lower()
        cases = [c for c in cases if needle in str(c.get("id", "")).lower()]

    if args.list:
        for case in cases:
            print(case["id"])
        return 0

    smallclue = Path(args.smallclue)
    if not smallclue.exists():
        print(f"smallclue executable not found: {smallclue}", file=sys.stderr)
        return 2

    total = len(cases)
    passed = 0
    failures: List[CaseResult] = []
    for case in cases:
        result = run_case(case, git_bin=args.git_bin, smallclue=smallclue)
        if result.ok:
            passed += 1
            continue
        failures.append(result)
        print(f"FAIL {result.case_id}: {result.reason}", file=sys.stderr)
        if result.details:
            print(result.details, file=sys.stderr)

    print(f"git-mutation parity: {passed}/{total} passed")
    if failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
