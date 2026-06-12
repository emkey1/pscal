#!/usr/bin/env python3
"""Export Aether reference/instruction documents as a separate corpus manifest."""

from __future__ import annotations

import argparse
import json
import pathlib


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_DOCS = [
    REPO_ROOT / "Docs" / "aether_for_llms_and_others.md",
    REPO_ROOT / "Docs" / "aether_for_llms_with_small_contexts.md",
]


def count_words(text: str) -> int:
    return len(text.split())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-json", type=pathlib.Path, required=True)
    args = parser.parse_args()

    items: list[dict[str, object]] = []
    for path in DEFAULT_DOCS:
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

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps({"items": items}, indent=2), encoding="utf-8")
    print(f"items={len(items)} -> {args.output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
