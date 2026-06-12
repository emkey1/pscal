#!/usr/bin/env python3
"""Verify, dedupe, and import external Aether corpus candidates into the repo."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_DEST_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates"
DEFAULT_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates_manifest.json"


def slugify(name: str) -> str:
    text = re.sub(r"[^A-Za-z0-9._-]+", "_", name.strip())
    text = text.strip("._-")
    return text or "candidate"


def load_existing_manifest(path: pathlib.Path) -> dict[str, Any]:
    if not path.exists():
        return {"items": []}
    return json.loads(path.read_text(encoding="utf-8"))


def existing_hashes(manifest: dict[str, Any]) -> set[str]:
    hashes: set[str] = set()
    for item in manifest.get("items", []):
        sha = item.get("sha256")
        if isinstance(sha, str) and sha:
            hashes.add(sha)
    return hashes


def verify_candidate(aether_bin: pathlib.Path, path: pathlib.Path) -> tuple[int, str, str]:
    proc = subprocess.run(
        [str(aether_bin), "--no-cache", str(path)],
        cwd="/tmp",
        text=True,
        capture_output=True,
        timeout=20,
    )
    return proc.returncode, proc.stdout, proc.stderr


def unique_target_path(dest_dir: pathlib.Path, base_name: str, sha256: str) -> pathlib.Path:
    stem = slugify(base_name)
    candidate = dest_dir / stem
    if not candidate.exists():
        return candidate
    if candidate.read_text(encoding="utf-8") == "":
        return candidate
    return dest_dir / f"{stem}_{sha256[:12]}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", help="candidate source files to import")
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--dest-dir", type=pathlib.Path, default=DEFAULT_DEST_DIR)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--include-in-training",
        action="store_true",
        help="promote imported candidates directly into the raw training corpus",
    )
    args = parser.parse_args()

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    args.dest_dir.mkdir(parents=True, exist_ok=True)
    manifest = load_existing_manifest(args.manifest)
    known_hashes = existing_hashes(manifest)
    imported: list[dict[str, Any]] = []
    rejected: list[dict[str, Any]] = []

    for raw_path in args.paths:
        path = pathlib.Path(raw_path)
        if not path.exists() or not path.is_file():
            rejected.append({"path": str(path), "reason": "missing"})
            continue

        source = path.read_text(encoding="utf-8")
        sha256 = hashlib.sha256(source.encode("utf-8")).hexdigest()
        if sha256 in known_hashes:
            rejected.append({"path": str(path), "reason": "duplicate_sha", "sha256": sha256})
            continue

        returncode, stdout, stderr = verify_candidate(args.aether_bin, path)
        if returncode != 0:
            rejected.append(
                {
                    "path": str(path),
                    "reason": "verification_failed",
                    "sha256": sha256,
                    "stderr": stderr,
                }
            )
            continue

        target = unique_target_path(args.dest_dir, path.name, sha256)
        target.write_text(source, encoding="utf-8")

        record = {
            "source_path": str(path),
            "repo_path": str(target.relative_to(REPO_ROOT)),
            "sha256": sha256,
            "stdout": stdout,
            "metadata": {
                "include_in_training": args.include_in_training,
                "lifecycle": "imported_candidate" if args.include_in_training else "quarantine",
            },
        }
        manifest.setdefault("items", []).append(record)
        known_hashes.add(sha256)
        imported.append(record)

    args.manifest.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps({"imported": imported, "rejected": rejected}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
