#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PASCAL_BIN = REPO_ROOT / "build/bin/pascal"
MANIFEST_PATH = Path(__file__).resolve().with_name("manifest.json")


class TestFailure(Exception):
    pass


def load_manifest():
    if not MANIFEST_PATH.exists():
        raise TestFailure(f"Manifest not found at {MANIFEST_PATH}")
    try:
        with MANIFEST_PATH.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
    except json.JSONDecodeError as exc:  # pragma: no cover - defensive
        raise TestFailure(f"Failed to parse manifest: {exc}") from exc

    tests = data.get("tests")
    if not isinstance(tests, list):
        raise TestFailure("Manifest is missing a list of tests")
    return tests


def matches_any(stderr_text, primary, alternates):
    if primary and primary in stderr_text:
        return True
    if alternates:
        for candidate in alternates:
            if candidate and candidate in stderr_text:
                return True
    return False


def run_test(entry):
    test_id = entry.get("id") or entry.get("name")
    if not test_id:
        raise TestFailure("Manifest entry missing 'id'")

    path_value = entry.get("path")
    if not path_value:
        raise TestFailure(f"{test_id}: Manifest entry missing 'path'")
    source_path = (REPO_ROOT / path_value).resolve()
    if not source_path.exists():
        raise TestFailure(f"{test_id}: Source not found at {source_path}")

    expect = entry.get("expect")
    if expect not in {"compile_success", "compile_error"}:
        raise TestFailure(f"{test_id}: Unsupported expect value: {expect}")

    stdout_sub = entry.get("stdout_substring")
    stderr_sub = entry.get("stderr_substring")
    stderr_any = entry.get("stderr_substrings_any") or []

    if not PASCAL_BIN.exists():
        raise TestFailure(f"Pascal compiler not found at {PASCAL_BIN}")

    cmd = [str(PASCAL_BIN), "--no-cache", str(source_path)]
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=str(source_path.parent),
        env=dict(os.environ, TERM=os.environ.get("TERM", "xterm-256color")),
    )

    stdout_text = proc.stdout.decode("utf-8", errors="replace")
    stderr_text = proc.stderr.decode("utf-8", errors="replace")

    if expect == "compile_success":
        if proc.returncode != 0:
            raise TestFailure(
                f"{test_id}: Expected success but compiler exited with {proc.returncode}.\n"
                f"stderr:\n{stderr_text.strip()}"
            )
        if stdout_sub and stdout_sub not in stdout_text:
            raise TestFailure(
                f"{test_id}: Expected stdout to contain {stdout_sub!r} but it did not."
            )
        if stderr_sub or stderr_any:
            if not matches_any(stderr_text, stderr_sub, stderr_any):
                raise TestFailure(
                    f"{test_id}: Expected stderr to contain {stderr_sub!r} but it did not."
                )
    else:
        if proc.returncode == 0:
            raise TestFailure(f"{test_id}: Expected failure but compiler succeeded")
        if not matches_any(stderr_text, stderr_sub, stderr_any):
            raise TestFailure(
                f"{test_id}: Expected stderr to contain {stderr_sub!r} but it did not.\n"
                f"stderr:\n{stderr_text.strip()}"
            )
        if stdout_sub and stdout_sub not in stdout_text:
            raise TestFailure(
                f"{test_id}: Expected stdout to contain {stdout_sub!r} but it did not."
            )


def main():
    try:
        tests = load_manifest()
        for entry in tests:
            run_test(entry)
    except TestFailure as exc:
        print(str(exc), file=sys.stdout)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
