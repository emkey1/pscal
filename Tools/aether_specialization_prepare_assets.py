#!/usr/bin/env python3
"""Prepare compiler-verified Aether specialization assets in one step."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_INSTRUCTION_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "seed_instruction_pairs.json"
DEFAULT_REPAIR_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "seed_repair_pairs.json"
DEFAULT_BENCHMARK_TASKS = REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks.json"


def run(argv: list[str]) -> None:
    subprocess.run(argv, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--instruction-manifest", type=pathlib.Path, default=DEFAULT_INSTRUCTION_MANIFEST)
    parser.add_argument("--repair-manifest", type=pathlib.Path, default=DEFAULT_REPAIR_MANIFEST)
    parser.add_argument("--benchmark-tasks", type=pathlib.Path, default=DEFAULT_BENCHMARK_TASKS,
                        help="benchmark task manifest used to de-contaminate training data")
    parser.add_argument("--include-benchmark-overlap", action="store_true", default=False,
                        help="train on records that reproduce benchmark outputs (contaminates tasks.json as a metric)")
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
            str(REPO_ROOT / "tools" / "aether_specialization_validate_corpus.py"),
            "--strict",
        ]
    )
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
    # v7: build real, compiler-verified instruction + repair supervision instead of
    # emitting empty JSONL. Each canonical corpus case is promoted to an instruction
    # pair (request -> verified Aether) alongside the seed instruction/repair pairs.
    # (v6 wrote empty JSONL here, so the model trained on bare corpus completions with
    # no instruction signal and its no-guide accuracy collapsed to 0/25.)
    build_dataset_cmd = [
        "python3",
        str(REPO_ROOT / "Tools" / "aether_specialization_build_dataset.py"),
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
    if not args.include_benchmark_overlap:
        build_dataset_cmd += ["--exclude-benchmark-tasks", str(args.benchmark_tasks)]
    run(build_dataset_cmd)

    def count_records(path: pathlib.Path) -> int:
        if not path.exists():
            return 0
        return sum(1 for line in path.read_text(encoding="utf-8").splitlines() if line.strip())

    instruction_records = count_records(instruction_jsonl)
    repair_records = count_records(repair_jsonl)
    if instruction_records == 0:
        raise SystemExit(
            "instruction JSONL is empty after build_dataset; refusing to prepare a "
            "no-supervision asset set (this was the v6 failure mode)."
        )

    summary_path = output_dir / "aether_training_mix.json"
    summary_path.write_text(
        json.dumps(
            {
                "raw_corpus": str(corpus_json),
                "reference_corpus": str(reference_json),
                "instruction_jsonl": str(instruction_jsonl),
                "repair_jsonl": str(repair_jsonl),
                "instruction_records": instruction_records,
                "repair_records": repair_records,
                "policy": (
                    "instruction-only SFT: corpus cases promoted to verified instruction "
                    "pairs + seed instruction/repair pairs. Raw corpus and small guide are "
                    "still exported for provenance but are NOT language-modeled as bare "
                    "completions (trainer --include-raw-corpus / --include-reference default off)."
                ),
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    print(f"corpus_json={corpus_json}")
    print(f"reference_json={reference_json}")
    print(f"instruction_jsonl={instruction_jsonl} records={instruction_records}")
    print(f"repair_jsonl={repair_jsonl} records={repair_records}")
    print(f"training_mix_json={summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
