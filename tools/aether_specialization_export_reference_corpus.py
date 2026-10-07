#!/usr/bin/env python3
"""Export Aether reference/instruction documents as a separate corpus manifest.

Ships one Aether guide (by default the MEDIUM guide, decision D10: it is the
hard-ceilinged, snippet-gated variant every current board uses; the small
guide's cuts must not silently remove teaching text from the corpus) plus a
generated **builtin reference** (the non-SDL
builtin surface, from the compiler's own `builtins_json`), so the training
corpus teaches the real builtin names/signatures and that the surface is
queryable -- not just the prose guide. The builtin reference is generated fresh
from the binary (never a stale checked-in copy) and skipped gracefully if the
binary is unavailable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

# Reuse the standalone builtin-reference generator (same tools/ dir).
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_specialization_corpus_policy as policy  # noqa: E402
from aether_export_builtins_reference import (  # noqa: E402
    DEFAULT_EXCLUDE,
    build as build_builtins,
    query_builtins,
    render_markdown as render_builtins_md,
)

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_DOCS_DIR = REPO_ROOT / "components" / "aether" / "docs"
DEFAULT_DOC = "aether_for_llms_medium_contexts.md"
FULL_GUIDE = "aether_for_llms_and_others.md"
_STAMP_RE = re.compile(r"^\*Guide version: ([0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]+)\*\s*$", re.MULTILINE)


def guide_stamp(text: str) -> str | None:
    match = _STAMP_RE.search(text)
    return match.group(1) if match else None


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
        "--docs-dir",
        type=pathlib.Path,
        default=DEFAULT_DOCS_DIR,
        help="directory holding the Aether guides (default: components/aether/docs)",
    )
    parser.add_argument(
        "--doc",
        action="append",
        default=None,
        help="guide to export: a file name inside --docs-dir or a path (repeatable; "
        f"default: {DEFAULT_DOC})",
    )
    parser.add_argument(
        "--include-full-guide",
        action="store_true",
        help=f"also include the full Aether guide ({FULL_GUIDE})",
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

    def resolve(doc: str) -> pathlib.Path:
        candidate = pathlib.Path(doc)
        return candidate if candidate.is_file() else args.docs_dir / doc

    docs = [resolve(doc) for doc in (args.doc or [DEFAULT_DOC])]
    if args.include_full_guide:
        docs.append(args.docs_dir / FULL_GUIDE)

    items: list[dict[str, object]] = []
    source_docs: list[dict[str, object]] = []
    for path in docs:
        if not path.is_file():
            raise SystemExit(f"reference doc not found: {policy.display_path(path)}")
        text = path.read_text(encoding="utf-8")
        stamp = guide_stamp(text)
        if stamp is None:
            raise SystemExit(f"{path.name}: no '*Guide version: ...*' stamp; refusing an unstamped reference doc")
        sha = hashlib.sha256(text.encode("utf-8")).hexdigest()
        items.append(
            {
                "path": policy.display_path(path),
                "kind": "aether_reference_corpus",
                "title": path.name,
                "guide_stamp": stamp,
                "sha256": sha,
                "content": text,
                "bytes": len(text.encode("utf-8")),
                "words": count_words(text),
            }
        )
        source_docs.append({"title": path.name, "guide_stamp": stamp, "sha256": sha})
        print(f"reference doc: {path.name} stamp {stamp} sha256 {sha[:12]}")

    if not args.no_builtins:
        item = builtin_reference_item(args.aether_bin)
        if item is not None:
            items.append(item)

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(
        json.dumps({"source_docs": source_docs, "items": items}, indent=2), encoding="utf-8"
    )
    print(f"items={len(items)} -> {args.output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
