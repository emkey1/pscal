#!/usr/bin/env python3
"""Prepare compiler-verified Aether specialization assets in one step."""

from __future__ import annotations

import argparse
import pathlib
import subprocess


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_INSTRUCTION_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "seed_instruction_pairs.json"
DEFAULT_REPAIR_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "seed_repair_pairs.json"


def run(argv: list[str]) -> None:
    subprocess.run(argv, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--instruction-manifest", type=pathlib.Path, default=DEFAULT_INSTRUCTION_MANIFEST)
    parser.add_argument("--repair-manifest", type=pathlib.Path, default=DEFAULT_REPAIR_MANIFEST)
    args = parser.parse_args()

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    corpus_json = output_dir / "aether_raw_corpus.json"
    reference_json = output_dir / "aether_reference_corpus.json"
    instruction_jsonl = output_dir / "aether_instruction_sft.jsonl"
    repair_jsonl = output_dir / "aether_repair_sft.jsonl"

    run(
        [
            "python3",
            str(REPO_ROOT / "tools" / "aether_specialization_export_corpus.py"),
            "--output-json",
            str(corpus_json),
        ]
    )
    run(
        [
            "python3",
            str(REPO_ROOT / "tools" / "aether_specialization_export_reference_corpus.py"),
            "--output-json",
            str(reference_json),
        ]
    )
    run(
        [
            "python3",
            str(REPO_ROOT / "tools" / "aether_specialization_build_dataset.py"),
            "--instruction-manifest",
            str(args.instruction_manifest),
            "--repair-manifest",
            str(args.repair_manifest),
            "--instruction-jsonl",
            str(instruction_jsonl),
            "--repair-jsonl",
            str(repair_jsonl),
            "--aether-bin",
            str(args.aether_bin),
        ]
    )

    print(f"corpus_json={corpus_json}")
    print(f"reference_json={reference_json}")
    print(f"instruction_jsonl={instruction_jsonl}")
    print(f"repair_jsonl={repair_jsonl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
