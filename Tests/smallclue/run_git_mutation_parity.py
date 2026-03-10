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
            env = None
            env_overrides = action.get("env")
            if env_overrides is not None:
                if not isinstance(env_overrides, dict):
                    raise RuntimeError("git action env must be an object")
                env = dict(os.environ)
                for key, value in env_overrides.items():
                    if not isinstance(key, str):
                        raise RuntimeError("git action env keys must be strings")
                    env[key] = str(value)
            proc = run_cmd([git_bin, *[str(a) for a in argv]], repo, env=env)
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
            "id": "rm_single_tracked_file_quiet",
            "mode": "repo",
            "git_argv": ["rm", "-q", "keep.txt"],
            "smallclue_argv": ["git", "rm", "-q", "keep.txt"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
            ],
        },
        {
            "id": "rm_cached_keeps_worktree_file",
            "mode": "repo",
            "git_argv": ["rm", "--cached", "-q", "keep.txt"],
            "smallclue_argv": ["git", "rm", "--cached", "-q", "keep.txt"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
            ],
        },
        {
            "id": "rm_recursive_directory",
            "mode": "repo",
            "git_argv": ["rm", "-r", "-q", "tree"],
            "smallclue_argv": ["git", "rm", "-r", "-q", "tree"],
            "actions": [
                {"op": "write", "path": "tree/a.txt", "text": "a\n"},
                {"op": "write", "path": "tree/sub/b.txt", "text": "b\n"},
                {"op": "git", "argv": ["add", "tree/a.txt", "tree/sub/b.txt"]},
                {"op": "git", "argv": ["commit", "-m", "add tree files"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
            ],
        },
        {
            "id": "mv_tracked_file",
            "mode": "repo",
            "git_argv": ["mv", "tracked.txt", "renamed.txt"],
            "smallclue_argv": ["git", "mv", "tracked.txt", "renamed.txt"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
                {"git_argv": ["ls-files"]},
            ],
        },
        {
            "id": "clean_force_removes_untracked_file",
            "mode": "repo",
            "git_argv": ["clean", "-f", "-q"],
            "smallclue_argv": ["git", "clean", "-f", "-q"],
            "actions": [{"op": "write", "path": "scratch.txt", "text": "scratch\n"}],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "clean_force_removes_untracked_dir_with_d",
            "mode": "repo",
            "git_argv": ["clean", "-f", "-d", "-q"],
            "smallclue_argv": ["git", "clean", "-f", "-d", "-q"],
            "actions": [{"op": "write", "path": "tmpdir/sub/file.txt", "text": "scratch\n"}],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "clean_x_removes_untracked_and_ignored",
            "mode": "repo",
            "git_argv": ["clean", "-f", "-x", "-q"],
            "smallclue_argv": ["git", "clean", "-f", "-x", "-q"],
            "actions": [
                {"op": "write", "path": ".gitignore", "text": "*.tmp\nignored/\n"},
                {"op": "git", "argv": ["add", ".gitignore"]},
                {"op": "git", "argv": ["commit", "-m", "add ignore rules"]},
                {"op": "write", "path": "ignored.tmp", "text": "ignored\n"},
                {"op": "write", "path": "plain.tmp", "text": "plain\n"},
                {"op": "write", "path": "ignored/deep/file.txt", "text": "deep\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "clean_X_removes_only_ignored",
            "mode": "repo",
            "git_argv": ["clean", "-f", "-X", "-d", "-q"],
            "smallclue_argv": ["git", "clean", "-f", "-X", "-d", "-q"],
            "actions": [
                {"op": "write", "path": ".gitignore", "text": "*.tmp\nignored/\n"},
                {"op": "git", "argv": ["add", ".gitignore"]},
                {"op": "git", "argv": ["commit", "-m", "add ignore rules"]},
                {"op": "write", "path": "ignored.tmp", "text": "ignored\n"},
                {"op": "write", "path": "plain.tmp", "text": "plain\n"},
                {"op": "write", "path": "ignored/deep/file.txt", "text": "deep\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "clean_dry_run_preserves_untracked",
            "mode": "repo",
            "git_argv": ["clean", "-n", "-d", "-q"],
            "smallclue_argv": ["git", "clean", "-n", "-d", "-q"],
            "actions": [
                {"op": "write", "path": "dry/file.txt", "text": "dry\n"},
                {"op": "write", "path": "dry.txt", "text": "dry\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "clean_pathspec_single_file",
            "mode": "repo",
            "git_argv": ["clean", "-f", "-q", "keep.tmp"],
            "smallclue_argv": ["git", "clean", "-f", "-q", "keep.tmp"],
            "actions": [
                {"op": "write", "path": "keep.tmp", "text": "keep\n"},
                {"op": "write", "path": "leave.tmp", "text": "leave\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "clean_pathspec_glob",
            "mode": "repo",
            "git_argv": ["clean", "-f", "-q", "*.tmp"],
            "smallclue_argv": ["git", "clean", "-f", "-q", "*.tmp"],
            "actions": [
                {"op": "write", "path": "a.tmp", "text": "a\n"},
                {"op": "write", "path": "b.log", "text": "b\n"},
                {"op": "write", "path": "dir/c.tmp", "text": "c\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "blame_default_tracked_file",
            "mode": "repo",
            "git_argv": ["blame", "tracked.txt"],
            "smallclue_argv": ["git", "blame", "tracked.txt"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "blame_line_porcelain_tracked_file",
            "mode": "repo",
            "git_argv": ["blame", "--line-porcelain", "tracked.txt"],
            "smallclue_argv": ["git", "blame", "--line-porcelain", "tracked.txt"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "ls_tree_head_default",
            "mode": "repo",
            "git_argv": ["ls-tree", "HEAD"],
            "smallclue_argv": ["git", "ls-tree", "HEAD"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "ls_tree_recursive_name_only",
            "mode": "repo",
            "git_argv": ["ls-tree", "-r", "--name-only", "HEAD"],
            "smallclue_argv": ["git", "ls-tree", "-r", "--name-only", "HEAD"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "ls_tree_path_entry_only",
            "mode": "repo",
            "git_argv": ["ls-tree", "HEAD", "tree"],
            "smallclue_argv": ["git", "ls-tree", "HEAD", "tree"],
            "actions": [
                {"op": "write", "path": "tree/a.txt", "text": "a\n"},
                {"op": "write", "path": "tree/sub/b.txt", "text": "b\n"},
                {"op": "git", "argv": ["add", "tree/a.txt", "tree/sub/b.txt"]},
                {"op": "git", "argv": ["commit", "-m", "add tree files for ls-tree"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "ls_tree_path_slash_contents",
            "mode": "repo",
            "git_argv": ["ls-tree", "HEAD", "tree/"],
            "smallclue_argv": ["git", "ls-tree", "HEAD", "tree/"],
            "actions": [
                {"op": "write", "path": "tree/a.txt", "text": "a\n"},
                {"op": "write", "path": "tree/sub/b.txt", "text": "b\n"},
                {"op": "git", "argv": ["add", "tree/a.txt", "tree/sub/b.txt"]},
                {"op": "git", "argv": ["commit", "-m", "add tree files for ls-tree slash"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "ls_tree_recursive_dirs_only",
            "mode": "repo",
            "git_argv": ["ls-tree", "-r", "-d", "HEAD", "tree"],
            "smallclue_argv": ["git", "ls-tree", "-r", "-d", "HEAD", "tree"],
            "actions": [
                {"op": "write", "path": "tree/a.txt", "text": "a\n"},
                {"op": "write", "path": "tree/sub/b.txt", "text": "b\n"},
                {"op": "git", "argv": ["add", "tree/a.txt", "tree/sub/b.txt"]},
                {"op": "git", "argv": ["commit", "-m", "add tree files for ls-tree dirs"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "describe_always_without_tags",
            "mode": "repo",
            "git_argv": ["describe", "--always"],
            "smallclue_argv": ["git", "describe", "--always"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "describe_annotated_tag_head",
            "mode": "repo",
            "git_argv": ["describe"],
            "smallclue_argv": ["git", "describe"],
            "actions": [
                {"op": "git", "argv": ["tag", "-a", "-m", "release", "v1.0.0"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "describe_long_after_tag",
            "mode": "repo",
            "git_argv": ["describe", "--long"],
            "smallclue_argv": ["git", "describe", "--long"],
            "actions": [
                {"op": "git", "argv": ["tag", "-a", "-m", "release", "v1.0.0"]},
                {"op": "append", "path": "tracked.txt", "text": "after tag\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "after tag"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-01-03T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-01-03T00:00:00Z",
                    },
                },
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "describe_dirty_suffix",
            "mode": "repo",
            "git_argv": ["describe", "--dirty"],
            "smallclue_argv": ["git", "describe", "--dirty"],
            "actions": [
                {"op": "git", "argv": ["tag", "-a", "-m", "release", "v1.0.0"]},
                {"op": "append", "path": "tracked.txt", "text": "dirty state\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "reflog_default_head",
            "mode": "repo",
            "git_argv": ["reflog"],
            "smallclue_argv": ["git", "reflog"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "reflog_max_count_one",
            "mode": "repo",
            "git_argv": ["reflog", "-n", "1"],
            "smallclue_argv": ["git", "reflog", "-n", "1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "reflog_date_raw",
            "mode": "repo",
            "git_argv": ["reflog", "--date=raw", "--max-count=2"],
            "smallclue_argv": ["git", "reflog", "--date=raw", "--max-count=2"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "reflog_show_main",
            "mode": "repo",
            "git_argv": ["reflog", "show", "main", "-n", "1"],
            "smallclue_argv": ["git", "reflog", "show", "main", "-n", "1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "merge_base_head_parent",
            "mode": "repo",
            "git_argv": ["merge-base", "HEAD", "HEAD~1"],
            "smallclue_argv": ["git", "merge-base", "HEAD", "HEAD~1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "merge_base_all_head_parent",
            "mode": "repo",
            "git_argv": ["merge-base", "--all", "HEAD", "HEAD~1"],
            "smallclue_argv": ["git", "merge-base", "--all", "HEAD", "HEAD~1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "merge_base_is_ancestor_true",
            "mode": "repo",
            "git_argv": ["merge-base", "--is-ancestor", "HEAD~1", "HEAD"],
            "smallclue_argv": ["git", "merge-base", "--is-ancestor", "HEAD~1", "HEAD"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "merge_base_is_ancestor_false",
            "mode": "repo",
            "git_argv": ["merge-base", "--is-ancestor", "HEAD", "HEAD~1"],
            "smallclue_argv": ["git", "merge-base", "--is-ancestor", "HEAD", "HEAD~1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cherry_basic_plus",
            "mode": "repo",
            "git_argv": ["cherry", "HEAD~1", "HEAD"],
            "smallclue_argv": ["git", "cherry", "HEAD~1", "HEAD"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cherry_verbose_abbrev",
            "mode": "repo",
            "git_argv": ["cherry", "-v", "--abbrev", "HEAD~1", "HEAD"],
            "smallclue_argv": ["git", "cherry", "-v", "--abbrev", "HEAD~1", "HEAD"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cherry_patch_equivalent_minus",
            "mode": "repo",
            "git_argv": ["cherry", "main", "topic"],
            "smallclue_argv": ["git", "cherry", "main", "topic"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "topic"]},
                {"op": "append", "path": "tracked.txt", "text": "equivalent change\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "topic equivalent change"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-01T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-01T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "append", "path": "tracked.txt", "text": "equivalent change\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "main equivalent change"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-02T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-02T00:00:00Z",
                    },
                },
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cherry_with_limit",
            "mode": "repo",
            "git_argv": ["cherry", "main", "topic", "topic~1"],
            "smallclue_argv": ["git", "cherry", "main", "topic", "topic~1"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "topic"]},
                {"op": "append", "path": "tracked.txt", "text": "topic one\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "topic one"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-03T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-03T00:00:00Z",
                    },
                },
                {"op": "append", "path": "tracked.txt", "text": "topic two\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "topic two"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-04T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-04T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_show_current_attached",
            "mode": "repo",
            "git_argv": ["branch", "--show-current"],
            "smallclue_argv": ["git", "branch", "--show-current"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_show_current_detached",
            "mode": "repo",
            "git_argv": ["branch", "--show-current"],
            "smallclue_argv": ["git", "branch", "--show-current"],
            "actions": [
                {"op": "git", "argv": ["checkout", "--detach", "HEAD~1"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_list_merged_default_head",
            "mode": "repo",
            "git_argv": ["branch", "--merged"],
            "smallclue_argv": ["git", "branch", "--merged"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-merged"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_list_no_merged_default_head",
            "mode": "repo",
            "git_argv": ["branch", "--no-merged"],
            "smallclue_argv": ["git", "branch", "--no-merged"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-q", "-b", "topic-diverge"]},
                {"op": "append", "path": "tracked.txt", "text": "diverge\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "diverge"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-05T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-05T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "-q", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_list_merged_with_explicit_commit",
            "mode": "repo",
            "git_argv": ["branch", "--merged=topic-diverge"],
            "smallclue_argv": ["git", "branch", "--merged=topic-diverge"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-q", "-b", "topic-diverge"]},
                {"op": "append", "path": "tracked.txt", "text": "diverge\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "diverge"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-05T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-05T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "-q", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_list_contains_explicit_commit",
            "mode": "repo",
            "git_argv": ["branch", "--contains=topic-diverge"],
            "smallclue_argv": ["git", "branch", "--contains=topic-diverge"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-q", "-b", "topic-diverge"]},
                {"op": "append", "path": "tracked.txt", "text": "diverge\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "diverge"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-05T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-05T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "-q", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_list_no_contains_explicit_commit",
            "mode": "repo",
            "git_argv": ["branch", "--no-contains=topic-diverge"],
            "smallclue_argv": ["git", "branch", "--no-contains=topic-diverge"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-q", "-b", "topic-diverge"]},
                {"op": "append", "path": "tracked.txt", "text": "diverge\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "diverge"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-05T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-05T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "-q", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "branch_list_points_at_head",
            "mode": "repo",
            "git_argv": ["branch", "--points-at=HEAD"],
            "smallclue_argv": ["git", "branch", "--points-at=HEAD"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-points"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "rev_parse_abbrev_ref_named_branch",
            "mode": "repo",
            "git_argv": ["rev-parse", "--abbrev-ref", "topic"],
            "smallclue_argv": ["git", "rev-parse", "--abbrev-ref", "topic"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "rev_parse_symbolic_full_name_named_branch",
            "mode": "repo",
            "git_argv": ["rev-parse", "--symbolic-full-name", "topic"],
            "smallclue_argv": ["git", "rev-parse", "--symbolic-full-name", "topic"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "rev_parse_abbrev_ref_detached_head",
            "mode": "repo",
            "git_argv": ["rev-parse", "--abbrev-ref", "HEAD"],
            "smallclue_argv": ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            "actions": [
                {"op": "git", "argv": ["checkout", "--detach", "HEAD~1"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "rev_parse_symbolic_full_name_detached_head",
            "mode": "repo",
            "git_argv": ["rev-parse", "--symbolic-full-name", "HEAD"],
            "smallclue_argv": ["git", "rev-parse", "--symbolic-full-name", "HEAD"],
            "actions": [
                {"op": "git", "argv": ["checkout", "--detach", "HEAD~1"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cat_file_type_head_commit",
            "mode": "repo",
            "git_argv": ["cat-file", "-t", "HEAD"],
            "smallclue_argv": ["git", "cat-file", "-t", "HEAD"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cat_file_size_head_blob",
            "mode": "repo",
            "git_argv": ["cat-file", "-s", "HEAD:tracked.txt"],
            "smallclue_argv": ["git", "cat-file", "-s", "HEAD:tracked.txt"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cat_file_pretty_tree",
            "mode": "repo",
            "git_argv": ["cat-file", "-p", "HEAD^{tree}"],
            "smallclue_argv": ["git", "cat-file", "-p", "HEAD^{tree}"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "cat_file_blob_legacy_mode",
            "mode": "repo",
            "git_argv": ["cat-file", "blob", "HEAD:tracked.txt"],
            "smallclue_argv": ["git", "cat-file", "blob", "HEAD:tracked.txt"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "stash_push_quiet",
            "mode": "repo",
            "git_argv": ["stash", "push", "-q", "-m", "stash-one"],
            "smallclue_argv": ["git", "stash", "push", "-q", "-m", "stash-one"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "stash me\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["stash", "list"]},
            ],
        },
        {
            "id": "stash_pop_quiet",
            "mode": "repo",
            "git_argv": ["stash", "pop", "-q"],
            "smallclue_argv": ["git", "stash", "pop", "-q"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "stash then pop\n"},
                {"op": "git", "argv": ["stash", "push", "-q", "-m", "seed-pop"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["stash", "list"]},
            ],
        },
        {
            "id": "stash_drop_specific_entry",
            "mode": "repo",
            "git_argv": ["stash", "drop", "-q", "stash@{1}"],
            "smallclue_argv": ["git", "stash", "drop", "-q", "stash@{1}"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "one\n"},
                {"op": "git", "argv": ["stash", "push", "-q", "-m", "seed-one"]},
                {"op": "append", "path": "tracked.txt", "text": "two\n"},
                {"op": "git", "argv": ["stash", "push", "-q", "-m", "seed-two"]},
            ],
            "checks": [
                {"git_argv": ["stash", "list"]},
            ],
        },
        {
            "id": "merge_fast_forward_branch",
            "mode": "repo",
            "git_argv": ["merge", "-q", "topic-ff"],
            "smallclue_argv": ["git", "merge", "-q", "topic-ff"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "topic-ff"]},
                {"op": "append", "path": "tracked.txt", "text": "topic ff\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-m", "topic fast-forward"]},
                {"op": "git", "argv": ["checkout", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
            ],
        },
        {
            "id": "merge_no_ff_creates_merge_commit",
            "mode": "repo",
            "git_argv": ["merge", "--no-ff", "-q", "topic-nff"],
            "smallclue_argv": ["git", "merge", "--no-ff", "-q", "topic-nff"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "topic-nff"]},
                {"op": "append", "path": "tracked.txt", "text": "topic nff\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-m", "topic no-ff"]},
                {"op": "git", "argv": ["checkout", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["rev-list", "--count", "--merges", "HEAD~1..HEAD"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "cherry_pick_branch_head",
            "mode": "repo",
            "git_argv": ["cherry-pick", "-n", "topic-cp"],
            "smallclue_argv": ["git", "cherry-pick", "-n", "topic-cp"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "topic-cp"]},
                {"op": "append", "path": "tracked.txt", "text": "topic cp\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-m", "topic cp"]},
                {"op": "git", "argv": ["checkout", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "revert_head_commit",
            "mode": "repo",
            "git_argv": ["revert", "-n", "HEAD"],
            "smallclue_argv": ["git", "revert", "-n", "HEAD"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "to-revert\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-m", "to revert"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "rebase_linear_on_main",
            "mode": "repo",
            "git_argv": ["rebase", "main"],
            "smallclue_argv": ["git", "rebase", "main"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "topic-rb"]},
                {"op": "append", "path": "tracked.txt", "text": "topic rb\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-m", "topic rebase commit"]},
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "append", "path": "keep.txt", "text": "main rb\n"},
                {"op": "git", "argv": ["add", "keep.txt"]},
                {"op": "git", "argv": ["commit", "-m", "main rebase base"]},
                {"op": "git", "argv": ["checkout", "topic-rb"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["rev-parse", "--abbrev-ref", "HEAD"]},
                {"git_argv": ["rev-list", "--count", "main..HEAD"]},
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "log_oneline_specific_revision",
            "mode": "repo",
            "git_argv": ["log", "--oneline", "-n", "1", "HEAD~1"],
            "smallclue_argv": ["git", "log", "--oneline", "-n", "1", "HEAD~1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_oneline_range",
            "mode": "repo",
            "git_argv": ["log", "--oneline", "main..topic-log"],
            "smallclue_argv": ["git", "log", "--oneline", "main..topic-log"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-q", "-b", "topic-log"]},
                {"op": "append", "path": "tracked.txt", "text": "topic log\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-q", "-m", "topic log commit"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-06T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-06T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "-q", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_oneline_all",
            "mode": "repo",
            "git_argv": ["log", "--oneline", "--all", "-n", "5"],
            "smallclue_argv": ["git", "log", "--oneline", "--all", "-n", "5"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-q", "-b", "topic-log-all"]},
                {"op": "append", "path": "tracked.txt", "text": "topic log all\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-q", "-m", "topic log all commit"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-03-07T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-03-07T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "-q", "main"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_pretty_format_subject",
            "mode": "repo",
            "git_argv": ["log", "-n", "1", "--pretty=format:%s"],
            "smallclue_argv": ["git", "log", "-n", "1", "--pretty=format:%s"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_pretty_format_author_committer",
            "mode": "repo",
            "git_argv": ["log", "-n", "1", "--format=%an <%ae>|%cn <%ce>"],
            "smallclue_argv": ["git", "log", "-n", "1", "--format=%an <%ae>|%cn <%ce>"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_oneline_decorate_mode_short",
            "mode": "repo",
            "git_argv": ["log", "--oneline", "--decorate=short", "-n", "1"],
            "smallclue_argv": ["git", "log", "--oneline", "--decorate=short", "-n", "1"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-log-deco"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_oneline_no_decorate_overrides_decorate",
            "mode": "repo",
            "git_argv": ["log", "--oneline", "--decorate", "--no-decorate", "-n", "1"],
            "smallclue_argv": ["git", "log", "--oneline", "--decorate", "--no-decorate", "-n", "1"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-log-nodeco"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_oneline_no_abbrev_commit",
            "mode": "repo",
            "git_argv": ["log", "--oneline", "--no-abbrev-commit", "-n", "1"],
            "smallclue_argv": ["git", "log", "--oneline", "--no-abbrev-commit", "-n", "1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_oneline_custom_abbrev",
            "mode": "repo",
            "git_argv": ["log", "--oneline", "--abbrev=12", "-n", "1"],
            "smallclue_argv": ["git", "log", "--oneline", "--abbrev=12", "-n", "1"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_format_h_no_abbrev_commit",
            "mode": "repo",
            "git_argv": ["log", "-n", "1", "--format=%h", "--no-abbrev-commit"],
            "smallclue_argv": ["git", "log", "-n", "1", "--format=%h", "--no-abbrev-commit"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "log_format_h_custom_abbrev",
            "mode": "repo",
            "git_argv": ["log", "-n", "1", "--format=%h", "--abbrev=12"],
            "smallclue_argv": ["git", "log", "-n", "1", "--format=%h", "--abbrev=12"],
            "actions": [],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
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
            "id": "commit_with_author_override",
            "mode": "repo",
            "git_argv": ["commit", "-q", "--author=Alt Author <alt@example.com>", "-m", "authored commit"],
            "smallclue_argv": ["git", "commit", "-q", "--author=Alt Author <alt@example.com>", "-m", "authored commit"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "author override\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%an <%ae>|%cn <%ce>"]},
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_with_signoff",
            "mode": "repo",
            "git_argv": ["commit", "-q", "-s", "-m", "signed commit"],
            "smallclue_argv": ["git", "commit", "-q", "-s", "-m", "signed commit"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "signoff\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["log", "-n", "1", "--pretty=format:%(trailers:key=Signed-off-by,valueonly)"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_reuse_message_short_option",
            "mode": "repo",
            "git_argv": ["commit", "-q", "-C", "HEAD"],
            "smallclue_argv": ["git", "commit", "-q", "-C", "HEAD"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "reuse source\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-q", "-m", "template message"],
                    "env": {
                        "GIT_AUTHOR_NAME": "Template Author",
                        "GIT_AUTHOR_EMAIL": "template@example.com",
                    },
                },
                {"op": "append", "path": "tracked.txt", "text": "reuse target\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["log", "-n", "1", "--pretty=%an <%ae>|%cn <%ce>"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_amend_reuse_message",
            "mode": "repo",
            "git_argv": ["commit", "-q", "--amend", "-C", "HEAD~1"],
            "smallclue_argv": ["git", "commit", "-q", "--amend", "-C", "HEAD~1"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "source base\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-q", "-m", "source reused message"],
                    "env": {
                        "GIT_AUTHOR_NAME": "Source Author",
                        "GIT_AUTHOR_EMAIL": "source@example.com",
                    },
                },
                {"op": "append", "path": "tracked.txt", "text": "target base\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-q", "-m", "target message"],
                    "env": {
                        "GIT_AUTHOR_NAME": "Target Author",
                        "GIT_AUTHOR_EMAIL": "target@example.com",
                    },
                },
                {"op": "append", "path": "tracked.txt", "text": "target amend payload\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["log", "-n", "1", "--pretty=%an <%ae>|%cn <%ce>"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_amend_with_message",
            "mode": "repo",
            "git_argv": ["commit", "-q", "--amend", "-m", "amended message"],
            "smallclue_argv": ["git", "commit", "-q", "--amend", "-m", "amended message"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "amend staged\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-q", "-m", "temporary message"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_amend_no_edit_with_staged_changes",
            "mode": "repo",
            "git_argv": ["commit", "-q", "--amend", "--no-edit"],
            "smallclue_argv": ["git", "commit", "-q", "--amend", "--no-edit"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "no edit base\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-q", "-m", "keep message"]},
                {"op": "append", "path": "tracked.txt", "text": "no edit amend\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_amend_no_changes_message_only",
            "mode": "repo",
            "git_argv": ["commit", "-q", "--amend", "-m", "retitle without changes"],
            "smallclue_argv": ["git", "commit", "-q", "--amend", "-m", "retitle without changes"],
            "actions": [],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%s"]},
                {"git_argv": ["rev-list", "--count", "HEAD"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "commit_amend_reset_author",
            "mode": "repo",
            "git_argv": ["commit", "-q", "--amend", "--reset-author", "-m", "reset author amend"],
            "smallclue_argv": ["git", "commit", "-q", "--amend", "--reset-author", "-m", "reset author amend"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "reset author base\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "git", "argv": ["commit", "-q", "--author=Legacy Author <legacy@example.com>", "-m", "legacy authored"]},
                {"op": "append", "path": "tracked.txt", "text": "reset author amend\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["log", "-n", "1", "--pretty=%an <%ae>|%cn <%ce>|%s"]},
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "reset_pathspec_unstage_single_file",
            "mode": "repo",
            "git_argv": ["reset", "-q", "--", "tracked.txt"],
            "smallclue_argv": ["git", "reset", "-q", "--", "tracked.txt"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "pathspec one\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "reset_pathspec_with_revision",
            "mode": "repo",
            "git_argv": ["reset", "-q", "HEAD~1", "--", "tracked.txt"],
            "smallclue_argv": ["git", "reset", "-q", "HEAD~1", "--", "tracked.txt"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "pathspec two\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["diff", "--cached", "--name-status"]},
                {"git_argv": ["show", ":tracked.txt"]},
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
            "id": "branch_force_reset_existing",
            "mode": "repo",
            "git_argv": ["branch", "-f", "topic-force", "HEAD~1"],
            "smallclue_argv": ["git", "branch", "-f", "topic-force", "HEAD~1"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-force"]},
                {"op": "append", "path": "tracked.txt", "text": "third\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "third"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-01-03T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-01-03T00:00:00Z",
                    },
                },
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "topic-force"]},
                {"git_argv": ["rev-parse", "--verify", "HEAD~1"]},
            ],
        },
        {
            "id": "branch_copy_existing",
            "mode": "repo",
            "git_argv": ["branch", "-c", "copy-src", "copy-dst"],
            "smallclue_argv": ["git", "branch", "-c", "copy-src", "copy-dst"],
            "actions": [
                {"op": "git", "argv": ["branch", "copy-src"]},
            ],
            "checks": [
                {"git_argv": ["show-ref", "--verify", "refs/heads/copy-dst"]},
                {"git_argv": ["rev-parse", "--verify", "copy-src"]},
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
            "id": "branch_delete_long_force_option",
            "mode": "repo",
            "git_argv": ["branch", "--delete", "--force", "doomed-long"],
            "smallclue_argv": ["git", "branch", "--delete", "--force", "doomed-long"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-q", "-b", "doomed-long"]},
                {"op": "append", "path": "tracked.txt", "text": "doomed\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {
                    "op": "git",
                    "argv": ["commit", "-m", "doomed work"],
                    "env": {
                        "GIT_AUTHOR_DATE": "2024-01-04T00:00:00Z",
                        "GIT_COMMITTER_DATE": "2024-01-04T00:00:00Z",
                    },
                },
                {"op": "git", "argv": ["checkout", "-q", "main"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/heads/doomed-long"]},
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
            "id": "branch_set_upstream_named_branch",
            "mode": "repo",
            "git_argv": ["branch", "--set-upstream-to=origin/topic-up", "topic-up"],
            "smallclue_argv": ["git", "branch", "--set-upstream-to=origin/topic-up", "topic-up"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-up"]},
                {"op": "git", "argv": ["clone", "--bare", ".", "../remote.git"]},
                {"op": "git", "argv": ["remote", "add", "origin", "../remote.git"]},
                {"op": "git", "argv": ["push", "-q", "origin", "main"]},
                {"op": "git", "argv": ["push", "-q", "origin", "topic-up"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--abbrev-ref", "--symbolic-full-name", "topic-up@{upstream}"]},
            ],
        },
        {
            "id": "branch_set_upstream_current_short_option",
            "mode": "repo",
            "git_argv": ["branch", "-u", "origin/topic-cur"],
            "smallclue_argv": ["git", "branch", "-u", "origin/topic-cur"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-cur"]},
                {"op": "git", "argv": ["clone", "--bare", ".", "../remote.git"]},
                {"op": "git", "argv": ["remote", "add", "origin", "../remote.git"]},
                {"op": "git", "argv": ["push", "-q", "origin", "main"]},
                {"op": "git", "argv": ["push", "-q", "origin", "topic-cur"]},
                {"op": "git", "argv": ["checkout", "-q", "topic-cur"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"]},
            ],
        },
        {
            "id": "branch_unset_upstream_named_branch",
            "mode": "repo",
            "git_argv": ["branch", "--unset-upstream", "topic-unset"],
            "smallclue_argv": ["git", "branch", "--unset-upstream", "topic-unset"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-unset"]},
                {"op": "git", "argv": ["clone", "--bare", ".", "../remote.git"]},
                {"op": "git", "argv": ["remote", "add", "origin", "../remote.git"]},
                {"op": "git", "argv": ["push", "-q", "origin", "main"]},
                {"op": "git", "argv": ["push", "-q", "origin", "topic-unset"]},
                {"op": "git", "argv": ["branch", "--set-upstream-to=origin/topic-unset", "topic-unset"]},
            ],
            "checks": [
                {"git_argv": ["for-each-ref", "--format=%(upstream:short)", "refs/heads/topic-unset"]},
            ],
        },
        {
            "id": "branch_unset_upstream_current_branch",
            "mode": "repo",
            "git_argv": ["branch", "--unset-upstream"],
            "smallclue_argv": ["git", "branch", "--unset-upstream"],
            "actions": [
                {"op": "git", "argv": ["branch", "topic-unset-cur"]},
                {"op": "git", "argv": ["clone", "--bare", ".", "../remote.git"]},
                {"op": "git", "argv": ["remote", "add", "origin", "../remote.git"]},
                {"op": "git", "argv": ["push", "-q", "origin", "main"]},
                {"op": "git", "argv": ["push", "-q", "origin", "topic-unset-cur"]},
                {"op": "git", "argv": ["checkout", "-q", "topic-unset-cur"]},
                {"op": "git", "argv": ["branch", "--set-upstream-to=origin/topic-unset-cur"]},
            ],
            "checks": [
                {"git_argv": ["for-each-ref", "--format=%(upstream:short)", "refs/heads/topic-unset-cur"]},
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
            "id": "checkout_path_from_index",
            "mode": "repo",
            "git_argv": ["checkout", "--", "tracked.txt"],
            "smallclue_argv": ["git", "checkout", "--", "tracked.txt"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "staged line\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
                {"op": "append", "path": "tracked.txt", "text": "unstaged line\n"},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["show", ":tracked.txt"]},
                {"git_argv": ["show", "HEAD:tracked.txt"]},
            ],
        },
        {
            "id": "checkout_treeish_path_updates_index_and_worktree",
            "mode": "repo",
            "git_argv": ["checkout", "HEAD~1", "--", "tracked.txt"],
            "smallclue_argv": ["git", "checkout", "HEAD~1", "--", "tracked.txt"],
            "actions": [
                {"op": "append", "path": "tracked.txt", "text": "new staged line\n"},
                {"op": "git", "argv": ["add", "tracked.txt"]},
            ],
            "checks": [
                {"git_argv": ["status", "--porcelain=v1"]},
                {"git_argv": ["show", ":tracked.txt"]},
                {"git_argv": ["show", "HEAD~1:tracked.txt"]},
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
