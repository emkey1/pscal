#!/usr/bin/env python3
"""Export a raw Aether corpus manifest from example files."""

from __future__ import annotations

import argparse
import json
import pathlib


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ROOTS = [
    REPO_ROOT / "Examples" / "aether" / "base",
    REPO_ROOT / "Examples" / "aether" / "showcase",
]


def looks_like_source(path: pathlib.Path) -> bool:
    if path.name.startswith("."):
        return False
    if path.suffix in {".json", ".md"}:
        return False
    return path.is_file()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-json", type=pathlib.Path, required=True)
    args = parser.parse_args()

    items: list[dict[str, str]] = []
    for root in DEFAULT_ROOTS:
        for path in sorted(root.rglob("*")):
            if not looks_like_source(path):
                continue
            text = path.read_text(encoding="utf-8")
            items.append(
                {
                    "path": str(path.relative_to(REPO_ROOT)),
                    "kind": "raw_aether_corpus",
                    "content": text,
                }
            )

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps({"items": items}, indent=2), encoding="utf-8")
    print(f"items={len(items)} -> {args.output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
