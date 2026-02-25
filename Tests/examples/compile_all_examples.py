#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TIMEOUT_SECONDS = 60.0

LANG_CONFIG = {
    "pascal": {
        "binary": REPO_ROOT / "build/bin/pascal",
        "source_suffixes": {".pas"},
    },
    "clike": {
        "binary": REPO_ROOT / "build/bin/clike",
        "source_suffixes": {".cl"},
    },
    "rea": {
        "binary": REPO_ROOT / "build/bin/rea",
        "source_suffixes": {".rea"},
    },
}

SKIP_SUFFIXES = {
    ".bmp",
    ".csv",
    ".gif",
    ".h",
    ".inc",
    ".jpeg",
    ".jpg",
    ".json",
    ".md",
    ".orig",
    ".pdf",
    ".png",
    ".txt",
    ".xml",
    ".yaml",
    ".yml",
}

SKIP_BASENAMES = {
    "CMakeLists.txt",
    "Makefile",
}


@dataclass
class CompileFailure:
    language: str
    path: Path
    returncode: int
    output: str


def is_hidden(path: Path) -> bool:
    return any(part.startswith(".") for part in path.parts)


def is_source_candidate(path: Path, source_suffixes: set) -> bool:
    if is_hidden(path) or not path.is_file():
        return False
    if path.name in SKIP_BASENAMES:
        return False
    suffix = path.suffix.lower()
    if suffix in SKIP_SUFFIXES:
        return False
    if suffix in source_suffixes:
        return True
    return suffix == ""


def discover_sources(root: Path, source_suffixes: set) -> List[Path]:
    paths: List[Path] = []
    for candidate in root.rglob("*"):
        if is_source_candidate(candidate, source_suffixes):
            paths.append(candidate)
    paths.sort()
    return paths


def compile_example(binary: Path, script_path: Path, timeout_seconds: float) -> subprocess.CompletedProcess:
    cmd = [str(binary), "--no-cache", "--dump-bytecode-only", str(script_path)]
    return subprocess.run(
        cmd,
        cwd=script_path.parent,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout_seconds,
        check=False,
    )


def run_compile_sweep(timeout_seconds: float) -> int:
    total = 0
    failures: List[CompileFailure] = []
    discovered: Dict[str, int] = {}

    for language, config in LANG_CONFIG.items():
        binary = config["binary"]
        root = REPO_ROOT / "Examples" / language
        source_suffixes = config["source_suffixes"]

        if not binary.exists():
            print(f"[FAIL] Missing frontend binary for {language}: {binary}")
            return 2
        if not root.exists():
            print(f"[FAIL] Missing examples directory for {language}: {root}")
            return 2

        sources = discover_sources(root, source_suffixes)
        discovered[language] = len(sources)

        for source in sources:
            total += 1
            relative = source.relative_to(REPO_ROOT)
            print(f"[RUN ] {relative}")
            try:
                result = compile_example(binary, source, timeout_seconds)
            except subprocess.TimeoutExpired as exc:
                output = (exc.stdout or "") + (exc.stderr or "")
                failures.append(
                    CompileFailure(language=language, path=source, returncode=124, output=output.strip())
                )
                print(f"[FAIL] {relative} (timeout)")
                continue

            if result.returncode != 0:
                failures.append(
                    CompileFailure(
                        language=language,
                        path=source,
                        returncode=result.returncode,
                        output=(result.stdout or "").strip(),
                    )
                )
                print(f"[FAIL] {relative} (exit={result.returncode})")
            else:
                print(f"[PASS] {relative}")

    print("\nDiscovery summary:")
    for language in ("pascal", "clike", "rea"):
        print(f"  {language}: {discovered.get(language, 0)} source file(s)")
    print(f"  total: {total} source file(s)")

    if failures:
        print(f"\nCompile failures: {len(failures)}")
        for failure in failures:
            relative = failure.path.relative_to(REPO_ROOT)
            print(f"\n[{failure.language}] {relative} (exit={failure.returncode})")
            if failure.output:
                lines = failure.output.splitlines()
                preview = lines[:40]
                for line in preview:
                    print(f"    {line}")
                if len(lines) > 40:
                    print(f"    ... ({len(lines) - 40} more lines)")
            else:
                print("    <no output>")
        return 1

    print("\nAll discovered examples compiled successfully with --dump-bytecode-only.")
    return 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compile all Pascal/CLike/Rea examples with --dump-bytecode-only."
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"Per-example compile timeout in seconds (default: {DEFAULT_TIMEOUT_SECONDS}).",
    )
    args = parser.parse_args(argv)

    os.chdir(REPO_ROOT)
    return run_compile_sweep(timeout_seconds=args.timeout)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
