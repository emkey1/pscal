#!/usr/bin/env python3
"""Export a raw Aether corpus manifest from example files."""

from __future__ import annotations

import argparse
import json
import pathlib
import re


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ROOTS = [
    REPO_ROOT / "Examples" / "aether" / "base",
    REPO_ROOT / "Examples" / "aether" / "showcase",
    REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates",
]
DEFAULT_MANIFEST = (
    REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates_manifest.json"
)


def looks_like_source(path: pathlib.Path) -> bool:
    if path.name.startswith("."):
        return False
    if path.suffix in {".json", ".md"}:
        return False
    return path.is_file()


NON_CANONICAL_PATTERNS = [
    r"\bwriteln\s*\(",
    r"\bwrite\s*\(",
    r"^\s*use\s+[A-Za-z_][A-Za-z0-9_]*\s*;",
    r"\bwhile\b",
    r"\bfor\b",
    r"\bpar\s*\{",
    r"\bmyself\.",
    r"\bTOON\b",
]


def looks_canonical_for_training(text: str) -> bool:
    filtered = "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("//")
    )
    for pattern in NON_CANONICAL_PATTERNS:
        if re.search(pattern, filtered, flags=re.MULTILINE):
            return False
    return True


def load_manifest_metadata(path: pathlib.Path) -> dict[str, dict]:
    if not path.exists():
        return {}
    payload = json.loads(path.read_text(encoding="utf-8"))
    out: dict[str, dict] = {}
    for item in payload.get("items", []):
        repo_path = item.get("repo_path")
        if not isinstance(repo_path, str):
            continue
        metadata = item.get("metadata")
        if isinstance(metadata, dict):
            out[repo_path] = metadata
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-json", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()

    manifest_metadata = load_manifest_metadata(args.manifest)
    items: list[dict[str, str]] = []
    for root in DEFAULT_ROOTS:
        for path in sorted(root.rglob("*")):
            if not looks_like_source(path):
                continue
            text = path.read_text(encoding="utf-8")
            if not looks_canonical_for_training(text):
                continue
            repo_path = str(path.relative_to(REPO_ROOT))
            record = {
                "path": repo_path,
                "kind": "raw_aether_corpus",
                "content": text,
            }
            metadata = manifest_metadata.get(repo_path)
            if metadata:
                record["metadata"] = metadata
            items.append(
                record
            )

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps({"items": items}, indent=2), encoding="utf-8")
    print(f"items={len(items)} -> {args.output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
