#!/usr/bin/env python3
"""Build compiler-verified Aether specialization datasets."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import tempfile
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"


def read_json(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


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


def write_jsonl(path: pathlib.Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record, ensure_ascii=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instruction-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--repair-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--instruction-jsonl", type=pathlib.Path, required=True)
    parser.add_argument("--repair-jsonl", type=pathlib.Path, required=True)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    args = parser.parse_args()

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    instruction_records = build_instruction_records(read_json(args.instruction_manifest), args.aether_bin)
    repair_records = build_repair_records(read_json(args.repair_manifest), args.aether_bin)

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
