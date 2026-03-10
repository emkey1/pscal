#!/usr/bin/env python3
"""Run parity checks for SmallClue git remote/network-style commands."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence


@dataclass
class CmdResult:
    returncode: int
    stdout: str
    stderr: str


@dataclass
class CaseResult:
    case_id: str
    ok: bool
    reason: str
    detail: str


def run_cmd(argv: Sequence[str], *, cwd: Path, env: Dict[str, str] | None = None) -> CmdResult:
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


def compare_streams(lhs: str, rhs: str) -> tuple[bool, str]:
    if lhs == rhs:
        return True, ""
    return False, f"expected:\n{lhs}\nactual:\n{rhs}"


def normalize_world_text(text: str, world: Dict[str, Path]) -> str:
    out = text
    replacements = [
        (str(world["remote"]), "{REMOTE}"),
        (str(world["repo"]), "{REPO}"),
        (str(world["seed"]), "{SEED}"),
        (str(world["root"]), "{ROOT}"),
    ]
    for src, dst in replacements:
        out = out.replace(src, dst)
    return out


def build_env() -> Dict[str, str]:
    env = os.environ.copy()
    env.setdefault("GIT_AUTHOR_NAME", "PSCAL Tester")
    env.setdefault("GIT_AUTHOR_EMAIL", "pscal@example.com")
    env.setdefault("GIT_COMMITTER_NAME", "PSCAL Tester")
    env.setdefault("GIT_COMMITTER_EMAIL", "pscal@example.com")
    # Keep fixture commit IDs stable between baseline and actual worlds.
    env.setdefault("GIT_AUTHOR_DATE", "2024-01-01T00:00:00Z")
    env.setdefault("GIT_COMMITTER_DATE", "2024-01-01T00:00:00Z")
    return env


def create_world(root: Path, git_bin: str, *, with_clone: bool) -> Dict[str, Path]:
    env = build_env()
    seed = root / "seed"
    remote = root / "remote.git"
    repo = root / "repo"
    seed.mkdir(parents=True, exist_ok=True)

    ensure_ok(run_cmd([git_bin, "init", "-b", "main"], cwd=seed, env=env), "seed init")
    ensure_ok(run_cmd([git_bin, "config", "user.name", "PSCAL Tester"], cwd=seed, env=env), "seed user.name")
    ensure_ok(run_cmd([git_bin, "config", "user.email", "pscal@example.com"], cwd=seed, env=env), "seed user.email")
    (seed / "README.md").write_text("seed\n", encoding="utf-8")
    ensure_ok(run_cmd([git_bin, "add", "README.md"], cwd=seed, env=env), "seed add")
    ensure_ok(run_cmd([git_bin, "commit", "-m", "initial"], cwd=seed, env=env), "seed commit")

    ensure_ok(run_cmd([git_bin, "init", "--bare", str(remote)], cwd=root, env=env), "remote init --bare")
    ensure_ok(run_cmd([git_bin, "remote", "add", "origin", str(remote)], cwd=seed, env=env), "seed remote add")
    ensure_ok(run_cmd([git_bin, "push", "-u", "origin", "main"], cwd=seed, env=env), "seed push")
    ensure_ok(run_cmd([git_bin, "--git-dir", str(remote), "symbolic-ref", "HEAD", "refs/heads/main"], cwd=root, env=env), "remote HEAD -> main")

    if with_clone:
        ensure_ok(run_cmd([git_bin, "clone", str(remote), str(repo)], cwd=root, env=env), "world clone")
        ensure_ok(run_cmd([git_bin, "config", "user.name", "PSCAL Tester"], cwd=repo, env=env), "repo user.name")
        ensure_ok(run_cmd([git_bin, "config", "user.email", "pscal@example.com"], cwd=repo, env=env), "repo user.email")

    return {"root": root, "seed": seed, "remote": remote, "repo": repo}


def replace_tokens(items: Sequence[str], world: Dict[str, Path]) -> List[str]:
    out: List[str] = []
    for item in items:
        value = (
            item.replace("{REMOTE}", str(world["remote"]))
            .replace("{REPO}", str(world["repo"]))
            .replace("{SEED}", str(world["seed"]))
        )
        out.append(value)
    return out


def apply_actions(world: Dict[str, Path], actions: List[Dict[str, object]], git_bin: str) -> None:
    env = build_env()
    for action in actions:
        op = str(action.get("op", ""))
        if op == "write":
            rel = str(action["path"])
            text = str(action.get("text", ""))
            target = world["repo"] / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
            continue
        if op == "append":
            rel = str(action["path"])
            text = str(action.get("text", ""))
            target = world["repo"] / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("a", encoding="utf-8") as handle:
                handle.write(text)
            continue
        if op == "remove":
            rel = str(action["path"])
            target = world["repo"] / rel
            if target.exists():
                target.unlink()
            continue
        if op == "git":
            argv = [str(x) for x in action.get("argv", [])]
            ensure_ok(run_cmd([git_bin, *replace_tokens(argv, world)], cwd=world["repo"], env=env), f"action git {' '.join(argv)}")
            continue
        if op == "seed_commit_push":
            rel = str(action.get("path", "seed_update.txt"))
            text = str(action.get("text", "update\n"))
            msg = str(action.get("message", "seed update"))
            target = world["seed"] / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("a", encoding="utf-8") as handle:
                handle.write(text)
            ensure_ok(run_cmd([git_bin, "add", rel], cwd=world["seed"], env=env), "seed add update")
            ensure_ok(run_cmd([git_bin, "commit", "-m", msg], cwd=world["seed"], env=env), "seed commit update")
            ensure_ok(run_cmd([git_bin, "push", "origin", "main"], cwd=world["seed"], env=env), "seed push update")
            continue
        if op == "seed_git":
            argv = [str(x) for x in action.get("argv", [])]
            ensure_ok(
                run_cmd([git_bin, *replace_tokens(argv, world)], cwd=world["seed"], env=env),
                f"action seed git {' '.join(argv)}",
            )
            continue
        raise RuntimeError(f"unsupported action op: {op}")


def world_cwd(world: Dict[str, Path], mode: str) -> Path:
    return world["root"] if mode == "root" else world["repo"]


def run_case(case: Dict[str, object], *, git_bin: str, smallclue: Path) -> CaseResult:
    case_id = str(case.get("id", "unnamed"))
    mode = str(case.get("mode", "repo"))
    compare_output = bool(case.get("compare_output", False))
    git_argv = [str(x) for x in case.get("git_argv", [])]
    sc_argv = [str(x) for x in case.get("smallclue_argv", [])]
    actions = list(case.get("actions", []))
    checks = list(case.get("checks", []))
    with_clone = mode != "root"

    with tempfile.TemporaryDirectory(prefix="pscal_git_remote_") as td:
        root = Path(td)
        baseline_world = create_world(root / "baseline", git_bin, with_clone=with_clone)
        actual_world = create_world(root / "actual", git_bin, with_clone=with_clone)

        apply_actions(baseline_world, actions, git_bin)
        apply_actions(actual_world, actions, git_bin)

        env = build_env()
        git_cmd = [git_bin, *replace_tokens(git_argv, baseline_world)]
        sc_cmd = [str(smallclue), *replace_tokens(sc_argv, actual_world)]
        proc_git = run_cmd(git_cmd, cwd=world_cwd(baseline_world, mode), env=env)
        proc_sc = run_cmd(sc_cmd, cwd=world_cwd(actual_world, mode), env=env)

        if proc_git.returncode != proc_sc.returncode:
            return CaseResult(case_id, False, "exit", f"git={proc_git.returncode} smallclue={proc_sc.returncode}")
        if compare_output:
            norm_git_out = normalize_world_text(proc_git.stdout, baseline_world)
            norm_sc_out = normalize_world_text(proc_sc.stdout, actual_world)
            ok, detail = compare_streams(norm_git_out, norm_sc_out)
            if not ok:
                return CaseResult(case_id, False, "stdout", detail)
            norm_git_err = normalize_world_text(proc_git.stderr, baseline_world)
            norm_sc_err = normalize_world_text(proc_sc.stderr, actual_world)
            ok, detail = compare_streams(norm_git_err, norm_sc_err)
            if not ok:
                return CaseResult(case_id, False, "stderr", detail)

        for check in checks:
            check_mode = str(check.get("mode", "repo"))
            check_argv = [str(x) for x in check.get("git_argv", [])]
            chk_git = run_cmd(
                [git_bin, *replace_tokens(check_argv, baseline_world)],
                cwd=world_cwd(baseline_world, check_mode),
                env=env,
            )
            chk_sc = run_cmd(
                [git_bin, *replace_tokens(check_argv, actual_world)],
                cwd=world_cwd(actual_world, check_mode),
                env=env,
            )
            if chk_git.returncode != chk_sc.returncode:
                return CaseResult(case_id, False, "check-exit", f"{check_argv}: git={chk_git.returncode} actual={chk_sc.returncode}")
            norm_chk_git_out = normalize_world_text(chk_git.stdout, baseline_world)
            norm_chk_sc_out = normalize_world_text(chk_sc.stdout, actual_world)
            ok, detail = compare_streams(norm_chk_git_out, norm_chk_sc_out)
            if not ok:
                return CaseResult(case_id, False, "check-stdout", f"{check_argv}\n{detail}")
            norm_chk_git_err = normalize_world_text(chk_git.stderr, baseline_world)
            norm_chk_sc_err = normalize_world_text(chk_sc.stderr, actual_world)
            ok, detail = compare_streams(norm_chk_git_err, norm_chk_sc_err)
            if not ok:
                return CaseResult(case_id, False, "check-stderr", f"{check_argv}\n{detail}")

    return CaseResult(case_id, True, "ok", "")


def build_cases() -> List[Dict[str, object]]:
    return [
        {
            "id": "clone_local_remote",
            "mode": "root",
            "git_argv": ["clone", "{REMOTE}", "cloned"],
            "smallclue_argv": ["git", "clone", "{REMOTE}", "cloned"],
            "checks": [
                {"mode": "root", "git_argv": ["-C", "cloned", "rev-parse", "--abbrev-ref", "HEAD"]},
            ],
        },
        {
            "id": "remote_add_and_get_url",
            "mode": "repo",
            "git_argv": ["remote", "add", "backup", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "backup", "{REMOTE}"],
            "checks": [
                {"git_argv": ["remote", "get-url", "backup"]},
            ],
        },
        {
            "id": "remote_add_fetch",
            "mode": "repo",
            "git_argv": ["remote", "add", "--fetch", "backup", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--fetch", "backup", "{REMOTE}"],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "refs/remotes/backup/main"]},
            ],
        },
        {
            "id": "remote_add_track",
            "mode": "repo",
            "git_argv": ["remote", "add", "--track", "main", "tracked", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--track", "main", "tracked", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.tracked.fetch"]},
            ],
        },
        {
            "id": "remote_add_track_fetch",
            "mode": "repo",
            "git_argv": ["remote", "add", "--fetch", "--track", "main", "trackedf", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--fetch", "--track", "main", "trackedf", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.trackedf.fetch"]},
                {"git_argv": ["rev-parse", "--verify", "refs/remotes/trackedf/main"]},
            ],
        },
        {
            "id": "remote_add_mirror_fetch",
            "mode": "repo",
            "git_argv": ["remote", "add", "--mirror=fetch", "mf", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--mirror=fetch", "mf", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.mf.fetch"]},
            ],
        },
        {
            "id": "remote_add_mirror_push",
            "mode": "repo",
            "git_argv": ["remote", "add", "--mirror=push", "mp", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--mirror=push", "mp", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get", "remote.mp.mirror"]},
                {"git_argv": ["config", "--get-all", "remote.mp.fetch"]},
            ],
        },
        {
            "id": "remote_add_mirror_push_track_error",
            "mode": "repo",
            "git_argv": ["remote", "add", "--mirror=push", "--track", "main", "mpe", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--mirror=push", "--track", "main", "mpe", "{REMOTE}"],
            "checks": [],
        },
        {
            "id": "remote_add_master_head",
            "mode": "repo",
            "git_argv": ["remote", "add", "--master", "main", "mh", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--master", "main", "mh", "{REMOTE}"],
            "checks": [
                {"git_argv": ["symbolic-ref", "refs/remotes/mh/HEAD"]},
            ],
        },
        {
            "id": "remote_add_master_mirror_error",
            "mode": "repo",
            "git_argv": ["remote", "add", "--mirror=fetch", "--master", "main", "mme", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--mirror=fetch", "--master", "main", "mme", "{REMOTE}"],
            "checks": [],
        },
        {
            "id": "remote_add_no_tags",
            "mode": "repo",
            "git_argv": ["remote", "add", "--no-tags", "nt", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--no-tags", "nt", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get", "remote.nt.tagOpt"]},
            ],
        },
        {
            "id": "remote_add_no_fetch_overrides_fetch",
            "mode": "repo",
            "git_argv": ["remote", "add", "--fetch", "--no-fetch", "nf", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--fetch", "--no-fetch", "nf", "{REMOTE}"],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/remotes/nf/main"]},
            ],
        },
        {
            "id": "remote_add_no_track_overrides_track",
            "mode": "repo",
            "git_argv": ["remote", "add", "--track", "main", "--no-track", "ntrk", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--track", "main", "--no-track", "ntrk", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.ntrk.fetch"]},
            ],
        },
        {
            "id": "remote_add_no_master_overrides_master",
            "mode": "repo",
            "git_argv": ["remote", "add", "--master", "main", "--no-master", "nmh", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--master", "main", "--no-master", "nmh", "{REMOTE}"],
            "checks": [
                {"git_argv": ["symbolic-ref", "--quiet", "refs/remotes/nmh/HEAD"]},
            ],
        },
        {
            "id": "remote_add_no_mirror_overrides_mirror",
            "mode": "repo",
            "git_argv": ["remote", "add", "--mirror=fetch", "--no-mirror", "nmr", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--mirror=fetch", "--no-mirror", "nmr", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get", "remote.nmr.mirror"]},
                {"git_argv": ["config", "--get-all", "remote.nmr.fetch"]},
            ],
        },
        {
            "id": "remote_add_tags",
            "mode": "repo",
            "git_argv": ["remote", "add", "--tags", "tg", "{REMOTE}"],
            "smallclue_argv": ["git", "remote", "add", "--tags", "tg", "{REMOTE}"],
            "checks": [
                {"git_argv": ["config", "--get", "remote.tg.tagOpt"]},
            ],
        },
        {
            "id": "remote_get_url_all_fetch",
            "mode": "repo",
            "compare_output": True,
            "git_argv": ["remote", "get-url", "--all", "origin"],
            "smallclue_argv": ["git", "remote", "get-url", "--all", "origin"],
            "actions": [
                {"op": "git", "argv": ["remote", "set-url", "--add", "origin", "https://example.invalid/extra.git"]},
            ],
            "checks": [],
        },
        {
            "id": "remote_get_url_all_push",
            "mode": "repo",
            "compare_output": True,
            "git_argv": ["remote", "get-url", "--push", "--all", "origin"],
            "smallclue_argv": ["git", "remote", "get-url", "--push", "--all", "origin"],
            "actions": [
                {"op": "git", "argv": ["remote", "set-url", "--push", "--add", "origin", "https://example.invalid/push1.git"]},
                {"op": "git", "argv": ["remote", "set-url", "--push", "--add", "origin", "https://example.invalid/push2.git"]},
            ],
            "checks": [],
        },
        {
            "id": "remote_set_url_add",
            "mode": "repo",
            "git_argv": ["remote", "set-url", "--add", "origin", "https://example.invalid/extra.git"],
            "smallclue_argv": ["git", "remote", "set-url", "--add", "origin", "https://example.invalid/extra.git"],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.origin.url"]},
            ],
        },
        {
            "id": "remote_rename_origin_to_upstream",
            "mode": "repo",
            "git_argv": ["remote", "rename", "origin", "upstream"],
            "smallclue_argv": ["git", "remote", "rename", "origin", "upstream"],
            "checks": [
                {"git_argv": ["config", "--get", "remote.upstream.url"]},
                {"git_argv": ["config", "--get", "remote.origin.url"]},
                {"git_argv": ["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"]},
            ],
        },
        {
            "id": "remote_remove_backup",
            "mode": "repo",
            "git_argv": ["remote", "remove", "backup"],
            "smallclue_argv": ["git", "remote", "remove", "backup"],
            "actions": [
                {"op": "git", "argv": ["remote", "add", "backup", "{REMOTE}"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get", "remote.backup.url"]},
            ],
        },
        {
            "id": "remote_set_url_delete",
            "mode": "repo",
            "git_argv": ["remote", "set-url", "--delete", "origin", "https://example\\.invalid/extra\\.git"],
            "smallclue_argv": ["git", "remote", "set-url", "--delete", "origin", "https://example\\.invalid/extra\\.git"],
            "actions": [
                {"op": "git", "argv": ["remote", "set-url", "--add", "origin", "https://example.invalid/extra.git"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.origin.url"]},
            ],
        },
        {
            "id": "remote_set_url_replace_old_regex",
            "mode": "repo",
            "git_argv": [
                "remote",
                "set-url",
                "origin",
                "https://example.invalid/replaced.git",
                "https://example\\.invalid/extra\\.git",
            ],
            "smallclue_argv": [
                "git",
                "remote",
                "set-url",
                "origin",
                "https://example.invalid/replaced.git",
                "https://example\\.invalid/extra\\.git",
            ],
            "actions": [
                {"op": "git", "argv": ["remote", "set-url", "--add", "origin", "https://example.invalid/extra.git"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.origin.url"]},
            ],
        },
        {
            "id": "remote_set_pushurl_add",
            "mode": "repo",
            "git_argv": ["remote", "set-url", "--push", "--add", "origin", "https://example.invalid/push-only.git"],
            "smallclue_argv": ["git", "remote", "set-url", "--push", "--add", "origin", "https://example.invalid/push-only.git"],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.origin.pushurl"]},
            ],
        },
        {
            "id": "remote_set_pushurl_delete_regex",
            "mode": "repo",
            "git_argv": ["remote", "set-url", "--push", "--delete", "origin", "https://example\\.invalid/push-only\\.git"],
            "smallclue_argv": ["git", "remote", "set-url", "--push", "--delete", "origin", "https://example\\.invalid/push-only\\.git"],
            "actions": [
                {"op": "git", "argv": ["remote", "set-url", "--push", "--add", "origin", "https://example.invalid/push-only.git"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.origin.pushurl"]},
            ],
        },
        {
            "id": "ls_remote_heads_main",
            "mode": "repo",
            "compare_output": True,
            "git_argv": ["ls-remote", "--heads", "origin", "main"],
            "smallclue_argv": ["git", "ls-remote", "--heads", "origin", "main"],
            "checks": [],
        },
        {
            "id": "ls_remote_root_path_head",
            "mode": "root",
            "compare_output": True,
            "git_argv": ["ls-remote", "{REMOTE}", "HEAD"],
            "smallclue_argv": ["git", "ls-remote", "{REMOTE}", "HEAD"],
            "checks": [],
        },
        {
            "id": "ls_remote_tags_single",
            "mode": "repo",
            "compare_output": True,
            "git_argv": ["ls-remote", "--tags", "origin", "v-remote-1"],
            "smallclue_argv": ["git", "ls-remote", "--tags", "origin", "v-remote-1"],
            "actions": [
                {"op": "git", "argv": ["tag", "v-remote-1"]},
                {"op": "git", "argv": ["push", "origin", "refs/tags/v-remote-1:refs/tags/v-remote-1"]},
            ],
            "checks": [],
        },
        {
            "id": "ls_remote_symref_head",
            "mode": "repo",
            "compare_output": True,
            "git_argv": ["ls-remote", "--symref", "origin", "HEAD"],
            "smallclue_argv": ["git", "ls-remote", "--symref", "origin", "HEAD"],
            "checks": [],
        },
        {
            "id": "ls_remote_exit_code_no_match",
            "mode": "repo",
            "git_argv": ["ls-remote", "--exit-code", "origin", "refs/heads/does-not-exist-*"],
            "smallclue_argv": ["git", "ls-remote", "--exit-code", "origin", "refs/heads/does-not-exist-*"],
            "checks": [],
        },
        {
            "id": "fetch_updates",
            "mode": "repo",
            "git_argv": ["fetch", "origin"],
            "smallclue_argv": ["git", "fetch", "origin"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "from-seed\n", "message": "seed update"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "fetch_verbose_progress_flags",
            "mode": "repo",
            "git_argv": ["fetch", "--verbose", "--progress", "origin"],
            "smallclue_argv": ["git", "fetch", "--verbose", "--progress", "origin"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "from-seed-verbose-progress\n", "message": "seed update verbose progress"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "fetch_no_all_overrides_all",
            "mode": "repo",
            "git_argv": ["fetch", "--all", "--no-all", "origin"],
            "smallclue_argv": ["git", "fetch", "--all", "--no-all", "origin"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "from-seed-no-all\n", "message": "seed update no-all"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "fetch_all_updates",
            "mode": "repo",
            "git_argv": ["fetch", "--all"],
            "smallclue_argv": ["git", "fetch", "--all"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "from-seed-all\n", "message": "seed update all"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "fetch_with_tags_option",
            "mode": "repo",
            "git_argv": ["fetch", "--tags", "origin"],
            "smallclue_argv": ["git", "fetch", "--tags", "origin"],
            "actions": [
                {"op": "seed_git", "argv": ["tag", "v-fetch-tags"]},
                {"op": "seed_git", "argv": ["push", "origin", "refs/tags/v-fetch-tags:refs/tags/v-fetch-tags"]},
            ],
            "checks": [
                {"git_argv": ["show-ref", "--verify", "--quiet", "refs/tags/v-fetch-tags"]},
            ],
        },
        {
            "id": "fetch_with_no_tags_option",
            "mode": "repo",
            "git_argv": ["fetch", "--no-tags", "origin"],
            "smallclue_argv": ["git", "fetch", "--no-tags", "origin"],
            "actions": [
                {"op": "seed_git", "argv": ["tag", "v-fetch-no-tags"]},
                {"op": "seed_git", "argv": ["push", "origin", "refs/tags/v-fetch-no-tags:refs/tags/v-fetch-no-tags"]},
            ],
            "checks": [
                {"git_argv": ["show-ref", "--verify", "--quiet", "refs/tags/v-fetch-no-tags"]},
            ],
        },
        {
            "id": "fetch_with_dash_n_no_tags",
            "mode": "repo",
            "git_argv": ["fetch", "-n", "origin"],
            "smallclue_argv": ["git", "fetch", "-n", "origin"],
            "actions": [
                {"op": "seed_git", "argv": ["tag", "v-fetch-dash-n"]},
                {"op": "seed_git", "argv": ["push", "origin", "refs/tags/v-fetch-dash-n:refs/tags/v-fetch-dash-n"]},
            ],
            "checks": [
                {"git_argv": ["show-ref", "--verify", "--quiet", "refs/tags/v-fetch-dash-n"]},
            ],
        },
        {
            "id": "fetch_with_dash_p_prune",
            "mode": "repo",
            "git_argv": ["fetch", "-p", "origin"],
            "smallclue_argv": ["git", "fetch", "-p", "origin"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "fetch-prune-topic"]},
                {"op": "git", "argv": ["push", "origin", "fetch-prune-topic"]},
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "git", "argv": ["fetch", "origin"]},
                {"op": "git", "argv": ["push", "origin", "--delete", "fetch-prune-topic"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/remotes/origin/fetch-prune-topic"]},
            ],
        },
        {
            "id": "fetch_no_prune_overrides_prune",
            "mode": "repo",
            "git_argv": ["fetch", "--prune", "--no-prune", "origin"],
            "smallclue_argv": ["git", "fetch", "--prune", "--no-prune", "origin"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "fetch-no-prune-topic"]},
                {"op": "git", "argv": ["push", "origin", "fetch-no-prune-topic"]},
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "git", "argv": ["fetch", "origin"]},
                {"op": "git", "argv": ["push", "origin", "--delete", "fetch-no-prune-topic"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/remotes/origin/fetch-no-prune-topic"]},
            ],
        },
        {
            "id": "remote_update_all",
            "mode": "repo",
            "git_argv": ["remote", "update"],
            "smallclue_argv": ["git", "remote", "update"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "remote-update-all\n", "message": "remote update all"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "remote_update_named_origin",
            "mode": "repo",
            "git_argv": ["remote", "update", "origin"],
            "smallclue_argv": ["git", "remote", "update", "origin"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "remote-update-origin\n", "message": "remote update origin"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "remote_update_prune_origin",
            "mode": "repo",
            "git_argv": ["remote", "update", "--prune", "origin"],
            "smallclue_argv": ["git", "remote", "update", "--prune", "origin"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "update-prune-topic"]},
                {"op": "git", "argv": ["push", "origin", "update-prune-topic"]},
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "git", "argv": ["fetch", "origin"]},
                {"op": "git", "argv": ["push", "origin", "--delete", "update-prune-topic"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/remotes/origin/update-prune-topic"]},
            ],
        },
        {
            "id": "remote_update_no_prune_overrides_prune",
            "mode": "repo",
            "git_argv": ["remote", "update", "--prune", "--no-prune", "origin"],
            "smallclue_argv": ["git", "remote", "update", "--prune", "--no-prune", "origin"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "update-no-prune-topic"]},
                {"op": "git", "argv": ["push", "origin", "update-no-prune-topic"]},
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "git", "argv": ["fetch", "origin"]},
                {"op": "git", "argv": ["push", "origin", "--delete", "update-no-prune-topic"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/remotes/origin/update-no-prune-topic"]},
            ],
        },
        {
            "id": "remote_prune_origin",
            "mode": "repo",
            "git_argv": ["remote", "prune", "origin"],
            "smallclue_argv": ["git", "remote", "prune", "origin"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "prune-topic"]},
                {"op": "git", "argv": ["push", "origin", "prune-topic"]},
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "git", "argv": ["fetch", "origin"]},
                {"op": "git", "argv": ["push", "origin", "--delete", "prune-topic"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/remotes/origin/prune-topic"]},
            ],
        },
        {
            "id": "remote_prune_origin_dry_run",
            "mode": "repo",
            "git_argv": ["remote", "prune", "--dry-run", "origin"],
            "smallclue_argv": ["git", "remote", "prune", "--dry-run", "origin"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "prune-dry-topic"]},
                {"op": "git", "argv": ["push", "origin", "prune-dry-topic"]},
                {"op": "git", "argv": ["checkout", "main"]},
                {"op": "git", "argv": ["fetch", "origin"]},
                {"op": "git", "argv": ["push", "origin", "--delete", "prune-dry-topic"]},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "--verify", "--quiet", "refs/remotes/origin/prune-dry-topic"]},
            ],
        },
        {
            "id": "remote_set_head_branch",
            "mode": "repo",
            "git_argv": ["remote", "set-head", "origin", "main"],
            "smallclue_argv": ["git", "remote", "set-head", "origin", "main"],
            "actions": [
                {"op": "git", "argv": ["fetch", "origin"]},
            ],
            "checks": [
                {"git_argv": ["symbolic-ref", "refs/remotes/origin/HEAD"]},
            ],
        },
        {
            "id": "remote_set_head_delete",
            "mode": "repo",
            "git_argv": ["remote", "set-head", "origin", "--delete"],
            "smallclue_argv": ["git", "remote", "set-head", "origin", "--delete"],
            "actions": [
                {"op": "git", "argv": ["remote", "set-head", "origin", "main"]},
            ],
            "checks": [
                {"git_argv": ["symbolic-ref", "--quiet", "refs/remotes/origin/HEAD"]},
            ],
        },
        {
            "id": "remote_set_head_auto",
            "mode": "repo",
            "git_argv": ["remote", "set-head", "origin", "--auto"],
            "smallclue_argv": ["git", "remote", "set-head", "origin", "--auto"],
            "actions": [
                {"op": "git", "argv": ["remote", "set-head", "origin", "--delete"]},
            ],
            "checks": [
                {"git_argv": ["symbolic-ref", "refs/remotes/origin/HEAD"]},
            ],
        },
        {
            "id": "remote_show_no_query",
            "mode": "repo",
            "git_argv": ["remote", "show", "-n", "origin"],
            "smallclue_argv": ["git", "remote", "show", "-n", "origin"],
            "checks": [
                {"git_argv": ["remote", "get-url", "origin"]},
            ],
        },
        {
            "id": "remote_show_query",
            "mode": "repo",
            "git_argv": ["remote", "show", "origin"],
            "smallclue_argv": ["git", "remote", "show", "origin"],
            "checks": [
                {"git_argv": ["remote", "get-url", "origin"]},
            ],
        },
        {
            "id": "remote_show_no_arg",
            "mode": "repo",
            "compare_output": True,
            "git_argv": ["remote", "show"],
            "smallclue_argv": ["git", "remote", "show"],
            "actions": [
                {"op": "git", "argv": ["remote", "add", "backup", "{REMOTE}"]},
            ],
            "checks": [],
        },
        {
            "id": "remote_show_multi",
            "mode": "repo",
            "git_argv": ["remote", "show", "origin", "backup"],
            "smallclue_argv": ["git", "remote", "show", "origin", "backup"],
            "actions": [
                {"op": "git", "argv": ["remote", "add", "backup", "{REMOTE}"]},
            ],
            "checks": [
                {"git_argv": ["remote", "get-url", "origin"]},
                {"git_argv": ["remote", "get-url", "backup"]},
            ],
        },
        {
            "id": "remote_set_branches_replace",
            "mode": "repo",
            "git_argv": ["remote", "set-branches", "origin", "main"],
            "smallclue_argv": ["git", "remote", "set-branches", "origin", "main"],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.origin.fetch"]},
            ],
        },
        {
            "id": "remote_set_branches_add",
            "mode": "repo",
            "git_argv": ["remote", "set-branches", "--add", "origin", "dev"],
            "smallclue_argv": ["git", "remote", "set-branches", "--add", "origin", "dev"],
            "actions": [
                {"op": "git", "argv": ["remote", "set-branches", "origin", "main"]},
            ],
            "checks": [
                {"git_argv": ["config", "--get-all", "remote.origin.fetch"]},
            ],
        },
        {
            "id": "pull_fast_forward",
            "mode": "repo",
            "git_argv": ["pull", "--ff-only", "origin", "main"],
            "smallclue_argv": ["git", "pull", "--ff-only", "origin", "main"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "pull-update\n", "message": "pull update"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "HEAD"]},
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "pull_verbose_progress_flags",
            "mode": "repo",
            "git_argv": ["pull", "--verbose", "--progress", "--ff-only", "origin", "main"],
            "smallclue_argv": ["git", "pull", "--verbose", "--progress", "--ff-only", "origin", "main"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "pull-verbose-progress\n", "message": "pull verbose progress"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "HEAD"]},
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "pull_default_upstream",
            "mode": "repo",
            "git_argv": ["pull"],
            "smallclue_argv": ["git", "pull"],
            "actions": [
                {"op": "seed_commit_push", "path": "README.md", "text": "pull-default\n", "message": "pull default"},
            ],
            "checks": [
                {"git_argv": ["rev-parse", "HEAD"]},
                {"git_argv": ["rev-parse", "refs/remotes/origin/main"]},
            ],
        },
        {
            "id": "rev_parse_upstream_abbrev_symbolic",
            "mode": "repo",
            "compare_output": True,
            "git_argv": ["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"],
            "smallclue_argv": ["git", "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"],
            "checks": [],
        },
        {
            "id": "pull_merge_commit",
            "mode": "repo",
            "git_argv": ["pull", "origin", "main"],
            "smallclue_argv": ["git", "pull", "origin", "main"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "local-diverge\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "local diverge"]},
                {"op": "seed_commit_push", "path": "README.md", "text": "remote-diverge\n", "message": "remote diverge"},
            ],
            "checks": [
                {"git_argv": ["rev-list", "--count", "--merges", "HEAD"]},
                {"git_argv": ["rev-parse", "--verify", "HEAD^2"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "pull_rebase_diverged",
            "mode": "repo",
            "git_argv": ["pull", "--rebase", "origin", "main"],
            "smallclue_argv": ["git", "pull", "--rebase", "origin", "main"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "local-rebase-diverge\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "local rebase diverge"]},
                {"op": "seed_commit_push", "path": "README.md", "text": "remote-rebase-diverge\n", "message": "remote rebase diverge"},
            ],
            "checks": [
                {"git_argv": ["rev-list", "--count", "--merges", "HEAD"]},
                {"git_argv": ["rev-parse", "--verify", "HEAD^"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "pull_rebase_equals_true_diverged",
            "mode": "repo",
            "git_argv": ["pull", "--rebase=true", "origin", "main"],
            "smallclue_argv": ["git", "pull", "--rebase=true", "origin", "main"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "local-rebase-eq-true\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "local rebase eq true"]},
                {"op": "seed_commit_push", "path": "README.md", "text": "remote-rebase-eq-true\n", "message": "remote rebase eq true"},
            ],
            "checks": [
                {"git_argv": ["rev-list", "--count", "--merges", "HEAD"]},
                {"git_argv": ["rev-parse", "--verify", "HEAD^"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "pull_rebase_equals_false_diverged",
            "mode": "repo",
            "git_argv": ["pull", "--rebase=false", "origin", "main"],
            "smallclue_argv": ["git", "pull", "--rebase=false", "origin", "main"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "local-rebase-eq-false\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "local rebase eq false"]},
                {"op": "seed_commit_push", "path": "README.md", "text": "remote-rebase-eq-false\n", "message": "remote rebase eq false"},
            ],
            "checks": [
                {"git_argv": ["rev-list", "--count", "--merges", "HEAD"]},
                {"git_argv": ["rev-parse", "--verify", "HEAD^2"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "pull_no_rebase_overrides_rebase",
            "mode": "repo",
            "git_argv": ["pull", "--rebase", "--no-rebase", "origin", "main"],
            "smallclue_argv": ["git", "pull", "--rebase", "--no-rebase", "origin", "main"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "local-no-rebase-diverge\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "local no-rebase diverge"]},
                {"op": "seed_commit_push", "path": "README.md", "text": "remote-no-rebase-diverge\n", "message": "remote no-rebase diverge"},
            ],
            "checks": [
                {"git_argv": ["rev-list", "--count", "--merges", "HEAD"]},
                {"git_argv": ["rev-parse", "--verify", "HEAD^2"]},
                {"git_argv": ["status", "--porcelain=v1"]},
            ],
        },
        {
            "id": "push_set_upstream",
            "mode": "repo",
            "git_argv": ["push", "-u", "origin"],
            "smallclue_argv": ["git", "push", "-u", "origin"],
            "actions": [
                {"op": "write", "path": "push.txt", "text": "push content\n"},
                {"op": "git", "argv": ["add", "push.txt"]},
                {"op": "git", "argv": ["commit", "-m", "push commit"]},
            ],
            "checks": [
                {"mode": "repo", "git_argv": ["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"]},
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "rev-list", "--count", "main"]},
            ],
        },
        {
            "id": "push_verbose_progress_flags",
            "mode": "repo",
            "git_argv": ["push", "--verbose", "--progress", "origin"],
            "smallclue_argv": ["git", "push", "--verbose", "--progress", "origin"],
            "actions": [
                {"op": "write", "path": "push_verbose.txt", "text": "push verbose content\n"},
                {"op": "git", "argv": ["add", "push_verbose.txt"]},
                {"op": "git", "argv": ["commit", "-m", "push verbose commit"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "rev-list", "--count", "main"]},
            ],
        },
        {
            "id": "push_no_force_overrides_force",
            "mode": "repo",
            "git_argv": ["push", "--force", "--no-force", "origin", "main:main"],
            "smallclue_argv": ["git", "push", "--force", "--no-force", "origin", "main:main"],
            "actions": [
                {"op": "write", "path": "push_no_force.txt", "text": "push no-force content\n"},
                {"op": "git", "argv": ["add", "push_no_force.txt"]},
                {"op": "git", "argv": ["commit", "-m", "push no force commit"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "rev-list", "--count", "main"]},
            ],
        },
        {
            "id": "push_follow_tags",
            "mode": "repo",
            "git_argv": ["push", "--follow-tags", "origin"],
            "smallclue_argv": ["git", "push", "--follow-tags", "origin"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "follow-tags\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "follow tags commit"]},
                {"op": "git", "argv": ["tag", "-a", "v-follow-1", "-m", "annotated follow tag"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "show-ref", "--verify", "refs/tags/v-follow-1"]},
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "rev-list", "--count", "main"]},
            ],
        },
        {
            "id": "push_no_follow_tags_overrides_follow_tags",
            "mode": "repo",
            "git_argv": ["push", "--follow-tags", "--no-follow-tags", "origin"],
            "smallclue_argv": ["git", "push", "--follow-tags", "--no-follow-tags", "origin"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "no-follow-tags\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "no follow tags commit"]},
                {"op": "git", "argv": ["tag", "-a", "v-no-follow-1", "-m", "annotated no follow tag"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "show-ref", "--verify", "--quiet", "refs/tags/v-no-follow-1"]},
            ],
        },
        {
            "id": "push_set_upstream_explicit_refspec",
            "mode": "repo",
            "git_argv": ["push", "-u", "origin", "main:main"],
            "smallclue_argv": ["git", "push", "-u", "origin", "main:main"],
            "actions": [
                {"op": "write", "path": "push2.txt", "text": "push2 content\n"},
                {"op": "git", "argv": ["add", "push2.txt"]},
                {"op": "git", "argv": ["commit", "-m", "push2 commit"]},
            ],
            "checks": [
                {"mode": "repo", "git_argv": ["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"]},
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "rev-list", "--count", "main"]},
            ],
        },
        {
            "id": "push_tags",
            "mode": "repo",
            "git_argv": ["push", "--tags", "origin"],
            "smallclue_argv": ["git", "push", "--tags", "origin"],
            "actions": [
                {"op": "git", "argv": ["tag", "v-test-1"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "show-ref", "--verify", "refs/tags/v-test-1"]},
            ],
        },
        {
            "id": "push_no_tags_overrides_tags",
            "mode": "repo",
            "git_argv": ["push", "--tags", "--no-tags", "origin"],
            "smallclue_argv": ["git", "push", "--tags", "--no-tags", "origin"],
            "actions": [
                {"op": "append", "path": "README.md", "text": "no-tags-push\n"},
                {"op": "git", "argv": ["add", "README.md"]},
                {"op": "git", "argv": ["commit", "-m", "no tags push commit"]},
                {"op": "git", "argv": ["tag", "v-no-tags-1"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "show-ref", "--verify", "--quiet", "refs/tags/v-no-tags-1"]},
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "rev-list", "--count", "main"]},
            ],
        },
        {
            "id": "push_delete_remote_tag",
            "mode": "repo",
            "git_argv": ["push", "--delete", "origin", "refs/tags/v-delete-1"],
            "smallclue_argv": ["git", "push", "--delete", "origin", "refs/tags/v-delete-1"],
            "actions": [
                {"op": "git", "argv": ["tag", "v-delete-1"]},
                {"op": "git", "argv": ["push", "origin", "refs/tags/v-delete-1:refs/tags/v-delete-1"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "rev-parse", "--verify", "--quiet", "refs/tags/v-delete-1"]},
            ],
        },
        {
            "id": "push_all_branches",
            "mode": "repo",
            "git_argv": ["push", "--all", "origin"],
            "smallclue_argv": ["git", "push", "--all", "origin"],
            "actions": [
                {"op": "git", "argv": ["checkout", "-b", "topic-all"]},
                {"op": "write", "path": "topic_all.txt", "text": "topic branch\n"},
                {"op": "git", "argv": ["add", "topic_all.txt"]},
                {"op": "git", "argv": ["commit", "-m", "topic branch commit"]},
                {"op": "git", "argv": ["checkout", "main"]},
            ],
            "checks": [
                {"mode": "root", "git_argv": ["--git-dir", "{REMOTE}", "show-ref", "--verify", "refs/heads/topic-all"]},
            ],
        },
    ]


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Run remote workflow parity between system git and SmallClue.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--git-bin", default="git", help="System git executable.")
    parser.add_argument(
        "--smallclue",
        default=str(Path(__file__).resolve().parents[2] / "build" / "bin" / "smallclue"),
        help="SmallClue executable path.",
    )
    parser.add_argument("--only", default="", help="Run only case IDs containing this substring.")
    parser.add_argument("--list", action="store_true", help="List case IDs and exit.")
    args = parser.parse_args(argv)

    smallclue = Path(args.smallclue)
    if not smallclue.exists():
        print(f"smallclue executable not found: {smallclue}", file=sys.stderr)
        return 2

    cases = build_cases()
    if args.only:
        needle = args.only.lower()
        cases = [c for c in cases if needle in str(c.get("id", "")).lower()]

    if args.list:
        for case in cases:
            print(case["id"])
        return 0

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
        if result.detail:
            print(result.detail, file=sys.stderr)

    print(f"git-remote parity: {passed}/{total} passed")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
