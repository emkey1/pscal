#!/usr/bin/env python3
"""Prepare compiler-verified Aether specialization assets in one step.

Order: strict corpus-layout validation, the recapture_expected.py --check
pre-flight (every golden in the manifest against the current binary), the raw
and reference exports, then the gated dataset build. Any failing step stops
the run before a dataset is written.
"""

from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import subprocess
import sys

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_specialization_corpus_policy as policy  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_INSTRUCTION_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "seed_instruction_pairs.json"
DEFAULT_REPAIR_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "seed_repair_pairs.json"
DEFAULT_CORPUS_MANIFEST = REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates_manifest.json"
TOOLS_DIR = REPO_ROOT / "tools"


def run(argv: list[str]) -> None:
    subprocess.run(argv, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--instruction-manifest", type=pathlib.Path, default=DEFAULT_INSTRUCTION_MANIFEST)
    parser.add_argument("--repair-manifest", type=pathlib.Path, default=DEFAULT_REPAIR_MANIFEST)
    parser.add_argument("--benchmark-tasks", type=pathlib.Path, action="append", default=None,
                        help="benchmark task manifest whose expected stdouts are dropped from training "
                        "(repeatable). Default: every board manifest (" + ", ".join(policy.BOARD_MANIFESTS)
                        + ", plus " + " and ".join(policy.FUTURE_BOARD_MANIFESTS) + " once they exist).")
    parser.add_argument("--include-benchmark-overlap", action="store_true", default=False,
                        help="train on records that reproduce benchmark outputs (contaminates the boards as a metric)")
    parser.add_argument("--corpus-manifest", type=pathlib.Path, default=DEFAULT_CORPUS_MANIFEST,
                        help="corpus manifest passed through to build_dataset (selects which corpus "
                        "candidates are promoted to instruction pairs).")
    parser.add_argument("--validate-manifest", type=pathlib.Path, default=None,
                        help="manifest used for the strict corpus-layout validation step "
                        "(default: the --corpus-manifest value).")
    parser.add_argument("--skip-recapture-check", action="store_true",
                        help="skip the recapture_expected.py --check pre-flight (iteration only; "
                        "aether_training_mix.json records that it was skipped)")
    parser.add_argument("--version", default=None,
                        help="dataset version stamp (YYYY-MM-DD-N); default: today-1. "
                        "Recorded in aether_training_mix.json alongside the aether "
                        "language version for traceability.")
    args = parser.parse_args()
    validate_manifest = args.validate_manifest if args.validate_manifest is not None else args.corpus_manifest

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")
    benchmark_tasks = args.benchmark_tasks or policy.default_board_manifests()
    checked_manifests = [] if args.include_benchmark_overlap else benchmark_tasks
    print(
        f"decontamination: {len(checked_manifests)} board manifest(s): "
        + (", ".join(policy.display_path(path) for path in checked_manifests) or "none (--include-benchmark-overlap)")
    )

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    corpus_json = output_dir / "aether_raw_corpus.json"
    reference_json = output_dir / "aether_reference_corpus.json"
    instruction_jsonl = output_dir / "aether_instruction_sft.jsonl"
    repair_jsonl = output_dir / "aether_repair_sft.jsonl"
    build_report_json = output_dir / "aether_build_report.json"
    recapture_report_json = output_dir / "aether_recapture_report.json"

    run(
        [
            sys.executable,
            str(TOOLS_DIR / "aether_specialization_validate_corpus.py"),
            "--strict",
            "--manifest",
            str(validate_manifest),
        ]
    )
    # Pre-flight: every golden in the manifest, trained or not, against the
    # binary that is about to build the dataset. Drift here is a compiler
    # behaviour change; it stops the run until a person has read the diff
    # (tools/recapture_expected.py --update) and accepted it (--accept).
    preflight = "skipped"
    if not args.skip_recapture_check:
        run(
            [
                sys.executable,
                str(TOOLS_DIR / "recapture_expected.py"),
                "--check",
                "--aether-bin",
                str(args.aether_bin),
                "--manifest",
                str(args.corpus_manifest),
                "--report-json",
                str(recapture_report_json),
            ]
        )
        preflight = "recapture_expected.py --check passed"
    run(
        [
            sys.executable,
            str(TOOLS_DIR / "aether_specialization_export_corpus.py"),
            "--output-json",
            str(corpus_json),
        ]
    )
    run(
        [
            sys.executable,
            str(TOOLS_DIR / "aether_specialization_export_reference_corpus.py"),
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
        sys.executable,
        str(TOOLS_DIR / "aether_specialization_build_dataset.py"),
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
        "--corpus-manifest",
        str(args.corpus_manifest),
        "--report-json",
        str(build_report_json),
    ]
    for path in checked_manifests:
        build_dataset_cmd += ["--exclude-benchmark-tasks", str(path)]
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

    build_report = json.loads(build_report_json.read_text(encoding="utf-8"))
    dataset_version = args.version or f"{datetime.date.today().isoformat()}-1"

    summary_path = output_dir / "aether_training_mix.json"
    # Paths are recorded relative to this file, so the summary never carries
    # the building host's directory layout.
    summary_path.write_text(
        json.dumps(
            {
                "version": dataset_version,
                "aether_version": build_report["aether_version"],
                "aether_sha256": build_report["aether_sha256"],
                "raw_corpus": corpus_json.name,
                "reference_corpus": reference_json.name,
                "instruction_jsonl": instruction_jsonl.name,
                "repair_jsonl": repair_jsonl.name,
                "build_report": build_report_json.name,
                "instruction_records": instruction_records,
                "repair_records": repair_records,
                "corpus_selection": build_report["corpus_selection"],
                "benchmark_overlap": build_report["benchmark_overlap"],
                "reference_similarity": {
                    key: build_report["reference_similarity"][key]
                    for key in ("shingle", "threshold", "tasks_with_reference", "records")
                } | {"pairs_over_threshold": build_report["reference_similarity"]["pairs_over_threshold"][:25]},
                "preflight": preflight,
                "gates": {
                    "exact_stdout_mismatches": sum(
                        len(v) for v in build_report["gates"]["exact_stdout_mismatches"].values()
                    ),
                    "returncode_failures": sum(
                        len(v) for v in build_report["gates"]["returncode_failures"].values()
                    ),
                    "golden_backstop_hits": len(build_report["gates"]["golden_backstop"]["records"])
                    + len(build_report["gates"]["golden_backstop"]["manifest_host_data"]),
                    "oracle_errors": len(build_report["gates"]["oracle_errors"]),
                },
                "policy": (
                    "instruction-only SFT: canonical corpus cases with an oracle are promoted to "
                    "verified instruction pairs + seed instruction/repair pairs; every record "
                    "reproduces its expected stdout exactly. Environment-dependent and harvested "
                    "(no-oracle) items never train. Raw corpus and reference guide are still "
                    "exported for provenance but are NOT language-modeled as bare completions "
                    "(trainer --include-raw-corpus / --include-reference default off)."
                ),
            },
            indent=2,
        )
        + "\n",
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
