#!/usr/bin/env python3
"""Prepare and sync Aether specialization assets to the Spark workspace."""

from __future__ import annotations

import argparse
import pathlib
import shlex
import subprocess


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_HOST = "claw@100.124.15.16"
DEFAULT_REMOTE_WORKSPACE = "$HOME/training/aether-qwen3-base"


def run_local(argv: list[str]) -> None:
    subprocess.run(argv, check=True)


def run_remote(host: str, script: str) -> None:
    subprocess.run(
        ["ssh", host, f"bash -lc {shlex.quote(script)}"],
        check=True,
        text=True,
    )


def resolve_remote_home(host: str) -> str:
    proc = subprocess.run(
        ["ssh", host, "bash -lc 'printf %s \"$HOME\"'"],
        check=True,
        text=True,
        capture_output=True,
    )
    return proc.stdout.strip()


def expand_remote_workspace(host: str, workspace: str) -> str:
    if "$HOME" not in workspace:
        return workspace
    return workspace.replace("$HOME", resolve_remote_home(host))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--remote-workspace", default=DEFAULT_REMOTE_WORKSPACE)
    parser.add_argument(
        "--local-output-dir",
        type=pathlib.Path,
        default=REPO_ROOT / "Tests" / "aether_specialization" / "out",
    )
    parser.add_argument(
        "--aether-bin",
        type=pathlib.Path,
        default=REPO_ROOT / "build" / "bin" / "aether",
    )
    args = parser.parse_args()

    args.local_output_dir.mkdir(parents=True, exist_ok=True)

    run_local(
        [
            "python3",
            str(REPO_ROOT / "tools" / "aether_specialization_prepare_assets.py"),
            "--output-dir",
            str(args.local_output_dir),
            "--aether-bin",
            str(args.aether_bin),
        ]
    )

    remote_workspace = expand_remote_workspace(args.host, args.remote_workspace)
    remote_data_dir = f"{remote_workspace}/data"
    run_remote(
        args.host,
        f'mkdir -p "{remote_data_dir}" "{args.remote_workspace}/logs" "{args.remote_workspace}/runs"',
    )

    local_dir = str(args.local_output_dir.resolve()) + "/"
    remote_target = f"{args.host}:{remote_data_dir}/"
    run_local(["rsync", "-a", "--delete", local_dir, remote_target])

    print(f"synced_local={args.local_output_dir}")
    print(f"synced_remote={remote_data_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
