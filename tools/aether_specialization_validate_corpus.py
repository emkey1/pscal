#!/usr/bin/env python3
"""Validate and triage the Aether specialization corpus layout."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
from collections import Counter


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_CORPUS_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates"
DEFAULT_FIXTURES_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "fixtures"
DEFAULT_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates_manifest.json"

SCRATCH_PATTERNS = [
    r"^aether_tmp\d+$",
    r"^aether_probe\d+$",
    r"^aether_new_probe\d+$",
    r"^aether_self_mut_probe\d+$",
    r"^aether_fileexists_",
    r"^prism_candidate_",
    r"^qnext\d+",
    r"^qwen\d+_",
    r"^nemo\d+",
    r"^cum_\d+$",
    r"^aa\d+$",
    r"^aa\d+_case\d+$",
    r"^[bcd]\d+$",
    r"^[cd]\d+$",
    r"^d\d+$",
]


def load_manifest(path: pathlib.Path) -> dict:
    if not path.exists():
        return {"items": []}
    return json.loads(path.read_text(encoding="utf-8"))


def is_source_candidate(path: pathlib.Path) -> bool:
    return path.is_file() and not path.name.startswith(".")


def classify_name(name: str) -> str:
    if name.endswith(".json"):
        return "fixture_in_corpus_dir"
    for pattern in SCRATCH_PATTERNS:
        if re.search(pattern, name):
            return "scratch_like"
    if name.endswith(".aether"):
        return "extensionful_source"
    if re.match(r"^\d+_", name) or re.match(r"^[A-Za-z]\d_", name):
        return "curated_named"
    return "other"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus-dir", type=pathlib.Path, default=DEFAULT_CORPUS_DIR)
    parser.add_argument("--fixtures-dir", type=pathlib.Path, default=DEFAULT_FIXTURES_DIR)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--report-json", type=pathlib.Path)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    manifest_items = manifest.get("items", [])

    repo_paths = [str(item.get("repo_path", "")) for item in manifest_items]
    repo_path_counts = Counter(repo_paths)
    duplicate_repo_paths = sorted(path for path, count in repo_path_counts.items() if path and count > 1)

    disk_files = sorted(path for path in args.corpus_dir.iterdir() if is_source_candidate(path))
    disk_names = {path.name for path in disk_files}

    manifest_paths: list[pathlib.Path] = []
    manifest_names: list[str] = []
    missing_manifest_files: list[str] = []
    outside_corpus_paths: list[str] = []
    for item in manifest_items:
        repo_path = item.get("repo_path")
        if not isinstance(repo_path, str) or not repo_path:
            continue
        abs_path = (REPO_ROOT / repo_path).resolve()
        manifest_paths.append(abs_path)
        manifest_names.append(abs_path.name)
        if args.corpus_dir.resolve() not in abs_path.parents:
            outside_corpus_paths.append(repo_path)
        if not abs_path.exists():
            missing_manifest_files.append(repo_path)

    manifest_name_set = set(manifest_names)
    disk_not_manifest = sorted(name for name in disk_names if name not in manifest_name_set)
    manifest_not_disk = sorted(name for name in manifest_name_set if name not in disk_names)

    fixture_files_in_corpus = sorted(path.name for path in disk_files if path.suffix == ".json")
    extensionful_sources = sorted(path.name for path in disk_files if path.suffix and path.suffix != ".json")

    scratch_like_on_disk = sorted(
        path.name for path in disk_files if classify_name(path.name) == "scratch_like"
    )
    scratch_like_in_manifest = sorted(
        name for name in manifest_names if classify_name(name) == "scratch_like"
    )

    unmanifested_curated = sorted(
        path.name
        for path in disk_files
        if path.name not in manifest_name_set and classify_name(path.name) == "curated_named"
    )
    unmanifested_support = sorted(
        path.name
        for path in disk_files
        if path.name not in manifest_name_set and classify_name(path.name) == "other"
    )

    report = {
        "summary": {
            "disk_files": len(disk_files),
            "manifest_items": len(manifest_items),
            "disk_not_manifest": len(disk_not_manifest),
            "manifest_not_disk": len(manifest_not_disk),
            "duplicate_repo_paths": len(duplicate_repo_paths),
            "fixture_files_in_corpus": len(fixture_files_in_corpus),
            "extensionful_sources": len(extensionful_sources),
            "scratch_like_on_disk": len(scratch_like_on_disk),
            "scratch_like_in_manifest": len(scratch_like_in_manifest),
        },
        "errors": {
            "missing_manifest_files": missing_manifest_files,
            "manifest_entries_outside_corpus_dir": sorted(set(outside_corpus_paths)),
            "disk_not_manifest": disk_not_manifest,
            "manifest_not_disk": manifest_not_disk,
            "duplicate_repo_paths": duplicate_repo_paths,
            "fixture_files_in_corpus_dir": fixture_files_in_corpus,
        },
        "warnings": {
            "extensionful_sources": extensionful_sources,
            "scratch_like_on_disk": scratch_like_on_disk,
            "scratch_like_in_manifest": scratch_like_in_manifest,
            "unmanifested_curated_named_files": unmanifested_curated,
            "unmanifested_support_files": unmanifested_support,
        },
    }

    if args.report_json:
        args.report_json.parent.mkdir(parents=True, exist_ok=True)
        args.report_json.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(json.dumps(report, indent=2))

    if args.strict:
        hard_fail = any(
            [
                missing_manifest_files,
                outside_corpus_paths,
                disk_not_manifest,
                manifest_not_disk,
                duplicate_repo_paths,
                fixture_files_in_corpus,
            ]
        )
        if hard_fail:
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
