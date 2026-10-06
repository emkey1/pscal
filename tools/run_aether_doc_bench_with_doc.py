#!/usr/bin/env python3
"""Run aether_doc_bench.py with an overridden guide path.

Kept for old command lines. It is now a thin wrapper over the harness's own
repeatable --doc NAME=PATH, which records the guide's path, stamp and sha256 in
the report like any other variant:

  run_aether_doc_bench_with_doc.py medium /path/to/guide.md [bench args...]

is exactly

  aether_doc_bench.py --doc medium=/path/to/guide.md [bench args...]

When the bench args carry no --docs, the run selects the overridden variant
alone (the harness default when --doc is given).
"""

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_doc_bench  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) < 2:
        raise SystemExit(
            "usage: run_aether_doc_bench_with_doc.py <variant> /path/to/guide.md [bench args...]"
        )
    variant, override_path, bench_args = argv[0], argv[1], argv[2:]
    if variant == "none":
        raise SystemExit("variant 'none' has no guide to override")
    return aether_doc_bench.main(["--doc", f"{variant}={override_path}", *bench_args])


if __name__ == "__main__":
    raise SystemExit(main())
