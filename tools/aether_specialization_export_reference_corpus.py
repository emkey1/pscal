#!/usr/bin/env python3
"""Export Aether reference/instruction documents as a separate corpus manifest.

Ships the Aether guide doc(s) plus a generated **builtin reference** (the non-SDL
builtin surface, from the compiler's own `builtins_json`), so the training
corpus teaches the real builtin names/signatures and that the surface is
queryable -- not just the prose guide. The builtin reference is generated fresh
from the binary (never a stale checked-in copy) and skipped gracefully if the
binary is unavailable.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

# Reuse the standalone builtin-reference generator (same tools/ dir).
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from aether_export_builtins_reference import (  # noqa: E402
    DEFAULT_EXCLUDE,
    build as build_builtins,
    query_builtins,
    render_markdown as render_builtins_md,
)

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_DOCS = [
    REPO_ROOT / "components" / "aether" / "docs" / "aether_for_llms_with_small_contexts.md",
]
OPTIONAL_DOCS = [
    REPO_ROOT / "components" / "aether" / "docs" / "aether_for_llms_and_others.md",
]


def count_words(text: str) -> int:
    return len(text.split())


def builtin_reference_item(aether_bin: pathlib.Path) -> dict[str, object] | None:
    """Generate the non-SDL builtin reference as a corpus item, or None if the
    binary is missing / the probe fails (corpus export must not hard-fail on it)."""
    if not aether_bin.exists():
        print(f"note: aether binary {aether_bin} not found; builtin reference skipped")
        return None
    try:
        builtins = query_builtins(aether_bin)
        _, dropped, documented, names_only = build_builtins(builtins, DEFAULT_EXCLUDE)
        md = render_builtins_md(documented, names_only, dropped, DEFAULT_EXCLUDE)
    except SystemExit as exc:  # query_builtins/sys.exit on probe failure
        print(f"note: builtin reference generation failed ({exc}); skipped")
        return None
    print(f"included builtin reference: {len(documented) + len(names_only)} non-SDL builtins "
          f"({len(documented)} documented)")
    return {
        "path": "tools/aether_export_builtins_reference.py",
        "kind": "aether_builtins_reference",
        "title": "aether_builtins_reference.md",
        "content": md,
        "bytes": len(md.encode("utf-8")),
        "words": count_words(md),
        "generated": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-json", type=pathlib.Path, required=True)
    parser.add_argument(
        "--include-full-guide",
        action="store_true",
        help="include the full Aether guide in the exported reference corpus",
    )
    parser.add_argument(
        "--aether-bin",
        type=pathlib.Path,
        default=DEFAULT_AETHER_BIN,
        help="aether binary used to generate the builtin reference (default: build/bin/aether)",
    )
    parser.add_argument(
        "--no-builtins",
        action="store_true",
        help="do not generate/include the builtin reference",
    )
    args = parser.parse_args()

    items: list[dict[str, object]] = []
    docs = list(DEFAULT_DOCS)
    if args.include_full_guide:
        docs.extend(OPTIONAL_DOCS)
    for path in docs:
        text = path.read_text(encoding="utf-8")
        items.append(
            {
                "path": str(path.relative_to(REPO_ROOT)),
                "kind": "aether_reference_corpus",
                "title": path.name,
                "content": text,
                "bytes": len(text.encode("utf-8")),
                "words": count_words(text),
            }
        )

    if not args.no_builtins:
        item = builtin_reference_item(args.aether_bin)
        if item is not None:
            items.append(item)

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps({"items": items}, indent=2), encoding="utf-8")
    print(f"items={len(items)} -> {args.output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
