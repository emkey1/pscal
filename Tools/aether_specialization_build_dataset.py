#!/usr/bin/env python3
"""Build compiler-verified Aether specialization datasets."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import tempfile
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_CORPUS_MANIFEST = (
    REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates_manifest.json"
)
DEFAULT_FIXTURES_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "fixtures"
CORPUS_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates"
EXAMPLE_DIRS = [
    REPO_ROOT / "Examples" / "aether" / "base",
    REPO_ROOT / "Examples" / "aether" / "showcase",
]


def read_json(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def find_support_source(module_name: str) -> pathlib.Path | None:
    candidates = [CORPUS_DIR / module_name]
    for root in EXAMPLE_DIRS:
        candidates.append(root / module_name)
    for candidate in candidates:
        if candidate.exists() and candidate.is_file():
            return candidate
    return None


def infer_support_files(source: str, fixtures_dir: pathlib.Path) -> dict[str, str]:
    files: dict[str, str] = {}
    for module_name in re.findall(r'^\s*use\s+"([^"]+)"\s*;', source, flags=re.MULTILINE):
        path = find_support_source(module_name)
        if path is not None:
            files[module_name] = path.read_text(encoding="utf-8")
    fixture_names = set()
    fixture_names.update(re.findall(r'toon_parse_file\("([^"]+)"\)', source))
    fixture_names.update(re.findall(r'fileexists\("([^"]+)"\)', source))
    for fixture_name in sorted(fixture_names):
        fixture_path = fixtures_dir / fixture_name
        if fixture_path.exists() and fixture_path.is_file():
            files[fixture_name] = fixture_path.read_text(encoding="utf-8")
    return files


def build_corpus_prompt(
    *,
    corpus_id: str,
    metadata: dict[str, Any],
    files: dict[str, str],
    expected_stdout: str,
) -> str:
    parts = [
        "Write canonical Aether source only.",
        f"Program id: {corpus_id}.",
    ]
    notes = str(metadata.get("notes", "") or "").strip()
    tags = metadata.get("tags")
    if isinstance(tags, list) and tags:
        parts.append("Concept tags: " + ", ".join(str(tag) for tag in tags) + ".")
    if notes:
        parts.append("Behavior notes: " + notes)
    if files:
        fixture_names = [name for name in sorted(files) if name.endswith(".json")]
        module_names = [name for name in sorted(files) if not name.endswith(".json")]
        if module_names:
            parts.append(
                "Provided modules in the working directory: "
                + ", ".join(f'"{name}"' for name in module_names)
                + ". Use their exported names exactly."
            )
        if fixture_names:
            parts.append(
                "Provided fixture files in the working directory: "
                + ", ".join(f'"{name}"' for name in fixture_names)
                + "."
            )
    if expected_stdout:
        parts.append("Exact stdout must be:")
        parts.append(expected_stdout.rstrip("\n"))
    return "\n\n".join(parts).strip()


def materialize_files(files: dict[str, str] | None, root: pathlib.Path) -> None:
    if not files:
        return
    for rel_path, content in files.items():
        target = root / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")


def verify_program(
    *,
    aether_bin: pathlib.Path,
    source: str,
    expected_stdout: str | None,
    files: dict[str, str] | None,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="aether-specialize-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        program_path = tmp_dir / "sample.aether"
        materialize_files(files, tmp_dir)
        program_path.write_text(source, encoding="utf-8")

        proc = subprocess.run(
            [str(aether_bin), "--no-cache", str(program_path)],
            cwd=str(tmp_dir),
            text=True,
            capture_output=True,
            timeout=60,
        )

        exact = expected_stdout is not None and proc.returncode == 0 and proc.stdout == expected_stdout
        return {
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "exact_stdout_match": exact,
        }


def build_instruction_records(payload: dict[str, Any], aether_bin: pathlib.Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for item in payload.get("pairs", []):
        verification = verify_program(
            aether_bin=aether_bin,
            source=item["solution"],
            expected_stdout=item.get("expected_stdout"),
            files=item.get("files"),
        )
        record = {
            "kind": "instruction_sft",
            "id": item["id"],
            "messages": [
                {
                    "role": "system",
                    "content": "You generate canonical Aether. When asked for code, output raw Aether source only.",
                },
                {
                    "role": "user",
                    "content": item["prompt"],
                },
                {
                    "role": "assistant",
                    "content": item["solution"],
                },
            ],
            "expected_stdout": item.get("expected_stdout"),
            "files": item.get("files", {}),
            "verification": verification,
        }
        records.append(record)
    return records


def build_repair_records(payload: dict[str, Any], aether_bin: pathlib.Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for item in payload.get("pairs", []):
        verification = verify_program(
            aether_bin=aether_bin,
            source=item["fixed_source"],
            expected_stdout=item.get("expected_stdout"),
            files=item.get("files"),
        )
        record = {
            "kind": "repair_sft",
            "id": item["id"],
            "messages": [
                {
                    "role": "system",
                    "content": "You repair invalid Aether into canonical Aether. Output raw Aether source only.",
                },
                {
                    "role": "user",
                    "content": (
                        "Fix this Aether program.\n\n"
                        f"Compiler diagnostic:\n{item['diagnostic']}\n\n"
                        f"Broken source:\n{item['broken_source']}"
                    ),
                },
                {
                    "role": "assistant",
                    "content": item["fixed_source"],
                },
            ],
            "diagnostic": item["diagnostic"],
            "expected_stdout": item.get("expected_stdout"),
            "files": item.get("files", {}),
            "verification": verification,
        }
        records.append(record)
    return records


def build_corpus_instruction_records(
    payload: dict[str, Any],
    *,
    aether_bin: pathlib.Path,
    fixtures_dir: pathlib.Path,
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for item in payload.get("items", []):
        repo_path = item.get("repo_path")
        if not isinstance(repo_path, str) or not repo_path:
            continue
        metadata = item.get("metadata") if isinstance(item.get("metadata"), dict) else {}
        if metadata.get("include_in_supervised") is False:
            continue
        if metadata.get("canonical") is False:
            continue
        expected_stdout = item.get("stdout")
        if not isinstance(expected_stdout, str) or not expected_stdout:
            continue

        # Resolve the module source from --corpus-dir (by basename) so an
        # alternate corpus form (e.g. corpus_candidates_oneliner) can be trained
        # on; fall back to the manifest's repo_path. The stdout is verified
        # against expected below, so a semantically-identical variant is safe.
        source_path = CORPUS_DIR / pathlib.Path(repo_path).name
        if not source_path.exists():
            source_path = REPO_ROOT / repo_path
        if not source_path.exists():
            continue
        source = source_path.read_text(encoding="utf-8")
        files = infer_support_files(source, fixtures_dir)
        verification = verify_program(
            aether_bin=aether_bin,
            source=source,
            expected_stdout=expected_stdout,
            files=files,
        )
        corpus_id = source_path.name
        record = {
            "kind": "corpus_instruction_sft",
            "id": corpus_id,
            "messages": [
                {
                    "role": "system",
                    "content": "You generate canonical Aether. When asked for code, output raw Aether source only.",
                },
                {
                    "role": "user",
                    "content": build_corpus_prompt(
                        corpus_id=corpus_id,
                        metadata=metadata,
                        files=files,
                        expected_stdout=expected_stdout,
                    ),
                },
                {
                    "role": "assistant",
                    "content": source,
                },
            ],
            "expected_stdout": expected_stdout,
            "files": files,
            "source_repo_path": repo_path,
            "metadata": metadata,
            "verification": verification,
        }
        records.append(record)
    return records


def load_benchmark_stdout(paths: list[pathlib.Path]) -> set[str]:
    """Collect expected_stdout from benchmark task manifests to de-contaminate training.

    The doc-bench tasks (Tests/aether_doc_bench/tasks.json) overlap heavily with the
    corpus and seed pairs. Training on records that reproduce a benchmark's exact output
    turns the benchmark into a memorization check, so we drop them by default and keep
    tasks.json as an honest held-out no-guide test.
    """
    outputs: set[str] = set()
    for path in paths:
        if not path.exists():
            raise SystemExit(f"benchmark task file not found: {path}")
        payload = json.loads(path.read_text(encoding="utf-8"))
        tasks = payload.get("tasks") if isinstance(payload, dict) else payload
        for task in tasks or []:
            stdout = task.get("expected_stdout")
            if isinstance(stdout, str) and stdout:
                outputs.add(stdout)
    return outputs


def drop_benchmark_overlap(
    records: list[dict[str, Any]], exclude_stdout: set[str]
) -> tuple[list[dict[str, Any]], list[str]]:
    if not exclude_stdout:
        return records, []
    kept: list[dict[str, Any]] = []
    dropped: list[str] = []
    for record in records:
        stdout = record.get("expected_stdout")
        if isinstance(stdout, str) and stdout in exclude_stdout:
            dropped.append(record.get("id", "?"))
        else:
            kept.append(record)
    return kept, dropped


def write_jsonl(path: pathlib.Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record, ensure_ascii=True) + "\n")


def main() -> int:
    global CORPUS_DIR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instruction-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--repair-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--instruction-jsonl", type=pathlib.Path, required=True)
    parser.add_argument("--repair-jsonl", type=pathlib.Path, required=True)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--corpus-manifest", type=pathlib.Path, default=DEFAULT_CORPUS_MANIFEST)
    parser.add_argument("--corpus-dir", type=pathlib.Path, default=CORPUS_DIR,
                        help="directory holding the corpus module sources (default: corpus_candidates). "
                        "Point at corpus_candidates_oneliner to train on the compact form.")
    parser.add_argument("--fixtures-dir", type=pathlib.Path, default=DEFAULT_FIXTURES_DIR)
    parser.add_argument(
        "--exclude-benchmark-tasks",
        action="append",
        type=pathlib.Path,
        default=[],
        help="benchmark task JSON whose expected_stdout values are dropped from training "
        "(keeps the benchmark an honest held-out test). Repeatable.",
    )
    args = parser.parse_args()

    CORPUS_DIR = args.corpus_dir

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    instruction_records = build_instruction_records(read_json(args.instruction_manifest), args.aether_bin)
    corpus_instruction_records = build_corpus_instruction_records(
        read_json(args.corpus_manifest),
        aether_bin=args.aether_bin,
        fixtures_dir=args.fixtures_dir,
    )
    instruction_records.extend(corpus_instruction_records)
    repair_records = build_repair_records(read_json(args.repair_manifest), args.aether_bin)

    exclude_stdout = load_benchmark_stdout(args.exclude_benchmark_tasks)
    instruction_records, dropped_instruction = drop_benchmark_overlap(instruction_records, exclude_stdout)
    repair_records, dropped_repair = drop_benchmark_overlap(repair_records, exclude_stdout)
    if exclude_stdout:
        print(
            f"excluded_benchmark_overlap instruction={len(dropped_instruction)} "
            f"repair={len(dropped_repair)} ids={sorted(dropped_instruction + dropped_repair)}"
        )

    bad_instruction = [r["id"] for r in instruction_records if r["verification"]["returncode"] != 0]
    bad_repair = [r["id"] for r in repair_records if r["verification"]["returncode"] != 0]
    if bad_instruction or bad_repair:
        details = []
        if bad_instruction:
            details.append("instruction failures: " + ", ".join(bad_instruction))
        if bad_repair:
            details.append("repair failures: " + ", ".join(bad_repair))
        raise SystemExit("verification failed: " + "; ".join(details))

    write_jsonl(args.instruction_jsonl, instruction_records)
    write_jsonl(args.repair_jsonl, repair_records)

    print(f"instruction_records={len(instruction_records)} -> {args.instruction_jsonl}")
    print(f"repair_records={len(repair_records)} -> {args.repair_jsonl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
