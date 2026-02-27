#!/usr/bin/env python3
import argparse
import os
import re
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
    "exsh": {
        "binary": REPO_ROOT / "build/bin/exsh",
        "source_suffixes": {".psh"},
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


def read_sdl_enabled_from_cache(cache_path: Path) -> bool:
    if not cache_path.exists():
        return False
    marker = re.compile(r"^SDL:BOOL=(ON|1|TRUE)$", re.IGNORECASE)
    with cache_path.open("r", encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            if marker.match(line.strip()):
                return True
    return False


def is_sdl_example(path: Path) -> bool:
    return any(part.lower() == "sdl" for part in path.parts)


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


def run_compile_sweep(
    timeout_seconds: float,
    languages: List[str],
    list_only: bool,
    skip_sdl_when_disabled: bool,
    fail_on_empty: bool,
) -> int:
    total = 0
    failures: List[CompileFailure] = []
    discovered: Dict[str, int] = {}
    skipped_sdl: Dict[str, int] = {}
    sdl_enabled = read_sdl_enabled_from_cache(REPO_ROOT / "build/CMakeCache.txt")

    for language in languages:
        config = LANG_CONFIG[language]
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
        if skip_sdl_when_disabled and not sdl_enabled:
            filtered_sources: List[Path] = []
            skipped = 0
            for source in sources:
                if is_sdl_example(source.relative_to(REPO_ROOT)):
                    skipped += 1
                else:
                    filtered_sources.append(source)
            sources = filtered_sources
            skipped_sdl[language] = skipped
        else:
            skipped_sdl[language] = 0
        discovered[language] = len(sources)

        if list_only:
            continue

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
    listed_total = 0
    for language in languages:
        discovered_count = discovered.get(language, 0)
        skipped_count = skipped_sdl.get(language, 0)
        listed_total += discovered_count
        if skipped_count > 0:
            print(f"  {language}: {discovered_count} source file(s) (skipped {skipped_count} SDL file(s))")
        else:
            print(f"  {language}: {discovered_count} source file(s)")
    print(f"  total: {listed_total} source file(s)")

    if fail_on_empty:
        empty_languages = [language for language in languages if discovered.get(language, 0) == 0]
        if empty_languages:
            joined = ", ".join(empty_languages)
            print(f"\n[FAIL] No discoverable example sources found for: {joined}")
            return 1

    if list_only:
        print("\nList-only mode complete (no compilation executed).")
        return 0

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
        description="Compile selected Pascal/CLike/exsh/Rea examples with --dump-bytecode-only."
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"Per-example compile timeout in seconds (default: {DEFAULT_TIMEOUT_SECONDS}).",
    )
    parser.add_argument(
        "--languages",
        nargs="+",
        choices=sorted(LANG_CONFIG.keys()),
        default=sorted(LANG_CONFIG.keys()),
        help="Subset of language front ends to process.",
    )
    parser.add_argument(
        "--list-only",
        action="store_true",
        help="Discover and report examples without compiling them.",
    )
    parser.add_argument(
        "--no-skip-sdl-when-disabled",
        action="store_true",
        help="Do not auto-skip examples under */sdl when SDL is disabled in build/CMakeCache.txt.",
    )
    parser.add_argument(
        "--fail-on-empty",
        action="store_true",
        help="Return non-zero if any selected language discovers zero source files.",
    )
    args = parser.parse_args(argv)

    os.chdir(REPO_ROOT)
    languages = sorted(set(args.languages))
    return run_compile_sweep(
        timeout_seconds=args.timeout,
        languages=languages,
        list_only=args.list_only,
        skip_sdl_when_disabled=not args.no_skip_sdl_when_disabled,
        fail_on_empty=args.fail_on_empty,
    )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
