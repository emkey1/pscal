#!/usr/bin/env python3
"""Export the raw Aether corpus from the corpus manifest.

Selection is the manifest's canonical flag, through the same
policy.canonical_exclusion the SFT builder uses (plus the raw pipeline's own
opt-out, include_in_training: false, and items without a golden, which are
support modules). There is no source-text heuristic any more: the regex that
dropped every `par` program and the numbered-name filter are gone.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_specialization_corpus_policy as policy  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ROOTS = [
    REPO_ROOT / "Examples" / "aether" / "base",
    REPO_ROOT / "Examples" / "aether" / "showcase",
]
DEFAULT_CORPUS_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates"
DEFAULT_MANIFEST = (
    REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates_manifest.json"
)


def looks_like_source(path: pathlib.Path) -> bool:
    if path.name.startswith("."):
        return False
    if path.suffix in {".json", ".md"}:
        return False
    return path.is_file()


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


def load_manifest_items(path: pathlib.Path) -> list[dict]:
    if not path.exists():
        return []
    payload = json.loads(path.read_text(encoding="utf-8"))
    return payload.get("items", [])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-json", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()

    manifest_items = load_manifest_items(args.manifest)
    items: list[dict[str, str]] = []
    missing_manifest_paths: list[str] = []
    excluded: collections.Counter[str] = collections.Counter()
    for item in manifest_items:
        repo_path = item.get("repo_path")
        if not isinstance(repo_path, str) or not repo_path:
            continue
        reason = policy.raw_exclusion(item)
        if reason is not None:
            excluded[reason] += 1
            continue
        metadata = policy.item_metadata(item)
        path = REPO_ROOT / repo_path
        if not path.exists():
            missing_manifest_paths.append(repo_path)
            continue
        if path.suffix == ".json" or DEFAULT_CORPUS_DIR.resolve() not in path.resolve().parents:
            excluded["outside_corpus_dir"] += 1
            continue
        record = {
            "path": repo_path,
            "kind": "raw_aether_corpus",
            "content": path.read_text(encoding="utf-8"),
        }
        if metadata:
            record["metadata"] = metadata
        items.append(record)

    if missing_manifest_paths:
        raise SystemExit(
            "missing manifest corpus files:\n" + "\n".join(sorted(missing_manifest_paths))
        )

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps({"items": items}, indent=2), encoding="utf-8")
    print(
        f"items={len(items)} -> {args.output_json} "
        + " ".join(f"excluded_{reason}={count}" for reason, count in sorted(excluded.items()))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
