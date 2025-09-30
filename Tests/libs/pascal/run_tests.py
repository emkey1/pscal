#!/usr/bin/env python3
"""Run the opt-in Pascal library test suite."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def find_repo_root(script_path: Path) -> Path:
    """Return the repository root based on this script's location."""
    return script_path.resolve().parents[3]


def _discover_ext_builtins(executable: Path) -> set[str]:
    """Return the set of extended builtin categories exposed by *executable*."""

    try:
        proc = subprocess.run(
            [str(executable), "--dump-ext-builtins"],
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return set()

    available: set[str] = set()
    for line in proc.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[0] == "category":
            available.add(parts[1])
    return available


def build_env(
    root: Path,
    tmp_dir: Path,
    home_dir: Path,
    available_builtins: set[str],
) -> dict[str, str]:
    """Construct the environment for running the Pascal test program."""
    env = os.environ.copy()

    lib_dir = root / "lib" / "pascal"
    existing = env.get("PASCAL_LIB_DIR")
    if existing:
        env.setdefault("PSCAL_LIB_DIR", existing)
    else:
        env["PASCAL_LIB_DIR"] = str(lib_dir)
        env.setdefault("PSCAL_LIB_DIR", str(lib_dir))

    env["PASCAL_TEST_TMPDIR"] = str(tmp_dir)
    env["HOME"] = str(home_dir)

    if available_builtins:
        env["PASCAL_TEST_EXT_BUILTINS"] = ",".join(sorted(available_builtins))
        env["PASCAL_TEST_HAS_YYJSON"] = "1" if "yyjson" in available_builtins else "0"

    return env


def _resolve_pascal_executable(root: Path) -> Path | None:
    """Return a usable path to the Pascal executable."""

    env_value = os.environ.get("PASCAL_BIN")
    candidates: list[Path] = []

    if env_value:
        env_path = Path(env_value)
        if env_path.is_dir():
            candidates.append(env_path / "pascal")
        candidates.append(env_path)

    candidates.append(root / "build" / "bin" / "pascal")

    which_path = shutil.which("pascal")
    if which_path:
        candidates.append(Path(which_path))

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate

    return None


def main() -> int:
    script_path = Path(__file__).resolve()
    root = find_repo_root(script_path)
    pascal_bin = _resolve_pascal_executable(root)
    if pascal_bin is None:
        print(
            "Pascal executable not found. Build the project or set PASCAL_BIN to "
            "a valid executable path.",
            file=sys.stderr,
        )
        return 1

    tmp_dir = Path(tempfile.mkdtemp(prefix="pascal_lib_tests_"))
    home_dir = tmp_dir / "home"
    home_dir.mkdir(parents=True, exist_ok=True)

    available_builtins = _discover_ext_builtins(pascal_bin)
    env = build_env(root, tmp_dir, home_dir, available_builtins)

    test_program = script_path.parent / "library_tests.pas"
    cmd = [str(pascal_bin), "--no-cache", str(test_program)]

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            cwd=str(root),
        )
    finally:
        try:
            shutil.rmtree(tmp_dir)
        except Exception:
            pass

    stdout = proc.stdout
    stderr = proc.stderr

    if stdout:
        print(stdout, end="")
    if stderr:
        print(stderr, file=sys.stderr, end="")

    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
