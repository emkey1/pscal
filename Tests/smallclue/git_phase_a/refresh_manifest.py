#!/usr/bin/env python3
"""Regenerate Phase A git parity manifest from deterministic fixture output."""

import json
import os
import subprocess
from pathlib import Path


CASES = [
    ("config_get_user_name", "config", ["config", "--get", "user.name"]),
    ("rev_parse_inside", "rev_parse", ["rev-parse", "--is-inside-work-tree"]),
    ("rev_parse_toplevel", "rev_parse", ["rev-parse", "--show-toplevel"]),
    ("rev_parse_gitdir", "rev_parse", ["rev-parse", "--git-dir"]),
    ("rev_parse_abbrev_head", "rev_parse", ["rev-parse", "--abbrev-ref", "HEAD"]),
    ("rev_parse_short_head", "rev_parse", ["rev-parse", "--short", "HEAD"]),
    ("rev_parse_verify_head", "rev_parse", ["rev-parse", "--verify", "HEAD"]),
    ("rev_parse_short12_feature", "rev_parse", ["rev-parse", "--short=12", "feature"]),
    ("rev_parse_verify_missing", "rev_parse", ["rev-parse", "--verify", "does-not-exist"]),
    ("symbolic_ref_head", "symbolic_ref", ["symbolic-ref", "HEAD"]),
    ("symbolic_ref_short_head", "symbolic_ref", ["symbolic-ref", "--short", "HEAD"]),
    ("show_ref_all", "show_ref", ["show-ref"]),
    ("show_ref_heads", "show_ref", ["show-ref", "--heads"]),
    ("show_ref_tags", "show_ref", ["show-ref", "--tags"]),
    ("rev_list_3", "rev_list", ["rev-list", "--max-count", "3", "HEAD"]),
    ("rev_list_reverse_2", "rev_list", ["rev-list", "--max-count", "2", "--reverse", "HEAD"]),
    ("ls_files_cached", "ls_files", ["ls-files"]),
    ("ls_files_others", "ls_files", ["ls-files", "--others", "--exclude-standard"]),
    ("status_porcelain", "status", ["status", "--porcelain=v1"]),
    ("status_porcelain_branch", "status", ["status", "--porcelain=v1", "-b"]),
    ("status_short_branch_all", "status", ["status", "--short", "-b", "--untracked-files=all"]),
    ("status_porcelain_no_untracked", "status", ["status", "--porcelain=v1", "--untracked-files=no"]),
    ("log_oneline_3", "log", ["log", "--oneline", "-n", "3"]),
    ("log_oneline_decorate_3", "log", ["log", "--oneline", "--decorate", "-n", "3"]),
    ("log_reverse_2", "log", ["log", "--oneline", "--max-count", "2", "--reverse"]),
    ("log_author_1", "log", ["log", "--author", "PSCAL Tester", "--oneline", "-n", "1"]),
    ("log_grep_update_1", "log", ["log", "--grep", "update", "--oneline", "-n", "1"]),
    ("show_oneline_head", "show", ["show", "--no-patch", "--pretty=oneline", "HEAD"]),
    ("show_name_only_head", "show", ["show", "--name-only", "--pretty=format:%s", "HEAD"]),
    ("show_name_status_head", "show", ["show", "--name-status", "--pretty=format:%s", "HEAD"]),
    ("show_stat_head", "show", ["show", "--stat", "--pretty=format:%s", "HEAD"]),
    ("diff_name_only_worktree", "diff", ["diff", "--name-only"]),
    ("diff_name_status_worktree", "diff", ["diff", "--name-status"]),
    ("diff_stat_worktree", "diff", ["diff", "--stat"]),
    ("diff_u1_worktree", "diff", ["diff", "-U1"]),
    ("diff_cached_name_only", "diff", ["diff", "--cached", "--name-only"]),
    ("diff_cached_name_status", "diff", ["diff", "--cached", "--name-status"]),
    ("diff_cached_stat", "diff", ["diff", "--cached", "--stat"]),
    ("diff_cached_u1", "diff", ["diff", "--cached", "-U1"]),
    ("diff_rev_name_only", "diff", ["diff", "HEAD~1", "HEAD", "--name-only"]),
    ("diff_rev_name_status", "diff", ["diff", "HEAD~1", "HEAD", "--name-status"]),
    ("diff_rev_stat", "diff", ["diff", "HEAD~1", "HEAD", "--stat"]),
    ("branch_list", "branch", ["branch", "--list"]),
    ("branch_list_a", "branch", ["branch", "--list", "-a"]),
    ("branch_list_v", "branch", ["branch", "--list", "-v"]),
    ("branch_list_vv", "branch", ["branch", "--list", "-vv"]),
    ("branch_list_pattern", "branch", ["branch", "--list", "f*"]),
    ("tag_list", "tag", ["tag", "--list"]),
    ("tag_list_pattern", "tag", ["tag", "--list", "v*"]),
    ("tag_list_n", "tag", ["tag", "--list", "-n"]),
    ("tag_list_n2", "tag", ["tag", "--list", "-n2"]),
]


def _load_fixture_metadata(setup_script: Path) -> dict:
    out = subprocess.check_output([str(setup_script)], text=True)
    meta = {}
    for line in out.strip().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            meta[key] = value
    return meta


def _path_variants(path: str) -> list:
    variants = {path, os.path.realpath(path)}
    extra = set()
    for candidate in list(variants):
        if candidate.startswith("/private/"):
            extra.add(candidate[len("/private") :])
        else:
            extra.add("/private" + candidate)
    variants |= extra
    return sorted(variants, key=len, reverse=True)


def _normalize_paths(text: str, path_variants: list) -> str:
    out = text
    for variant in path_variants:
        out = out.replace(variant, "${REPO_ROOT}")
    return out


def main() -> int:
    root = Path(__file__).resolve().parents[3]
    setup_script = root / "Tests/smallclue/git_phase_a/setup_phase_a_fixture.sh"
    manifest_path = root / "Tests/smallclue/git_phase_a/manifest.json"

    meta = _load_fixture_metadata(setup_script)
    repo_root = meta["REPO_ROOT"]
    variants = _path_variants(repo_root)

    tests = []
    for test_id, category, git_argv in CASES:
        proc = subprocess.run(
            ["git", *git_argv],
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        tests.append(
            {
                "id": test_id,
                "category": category,
                "git_argv": git_argv,
                "smallclue_argv": ["git", *git_argv],
                "expected_exit": proc.returncode,
                "expected_stdout": _normalize_paths(proc.stdout, variants),
                "expected_stderr": _normalize_paths(proc.stderr, variants),
                "comparison": {"stdout": "exact", "stderr": "exact", "exit": "exact"},
            }
        )

    manifest = {
        "version": 1,
        "suite": "smallclue_git_parity",
        "description": "Parity matrix for SmallClue git applet versus system git (local-repo commands only).",
        "fixture": {
            "id": "phase_a_baseline_dirty",
            "setup_script": "Tests/smallclue/git_phase_a/setup_phase_a_fixture.sh",
            "repo_root_token": "${REPO_ROOT}",
            "tokens": ["REPO_ROOT", "HEAD_OID", "HEAD_SHORT", "FEATURE_SHORT12", "TAG_TARGET"],
            "notes": [
                "Fixture is deterministic (fixed author/committer names and timestamps).",
                "Harness must run all commands with cwd=${REPO_ROOT}.",
                "SmallClue invocation should be: smallclue <smallclue_argv...>.",
                "Baseline invocation should be: git <git_argv...>.",
                "Replace ${REPO_ROOT} token in expectations before comparison.",
            ],
        },
        "tests": tests,
    }

    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {manifest_path} ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
