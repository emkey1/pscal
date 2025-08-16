#!/usr/bin/env python3
"""Locate the source file associated with a Pscal cache entry."""

import argparse
import os
from pathlib import Path

FNV_OFFSET = 2166136261
FNV_PRIME = 16777619

def hash_path(path: str) -> int:
    """Replicate the hash function used by Pscal's cache."""
    h = FNV_OFFSET
    for b in path.encode("utf-8"):
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFF
    return h

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Find source file(s) matching a Pscal cache file."
    )
    parser.add_argument("cache_file", help="Path to a file in ~/.pscal_cache")
    parser.add_argument(
        "search_dirs",
        nargs="*",
        default=["."],
        help="Directories to recursively search for source files",
    )
    args = parser.parse_args()

    cache_path = Path(args.cache_file).expanduser()
    if not cache_path.name.endswith(".bc"):
        raise SystemExit("Cache file should have a .bc extension")
    try:
        target_hash = int(cache_path.stem)
    except ValueError:
        raise SystemExit("Cache file name must be numeric like '<hash>.bc'")

    matches: list[str] = []
    for directory in args.search_dirs:
        base = Path(directory)
        if not base.is_dir():
            continue
        for file_path in base.rglob("*"):
            if not file_path.is_file():
                continue
            rel_str = file_path.relative_to(base).as_posix()
            abs_str = str(file_path.resolve())
            if hash_path(rel_str) == target_hash or hash_path(abs_str) == target_hash:
                matches.append(str(file_path))

    if matches:
        for m in matches:
            print(m)
    else:
        print("No matching source files found.")

if __name__ == "__main__":
    main()
