#!/usr/bin/env python3
"""Build compiler-verified Aether specialization datasets.

Every record is gated, and the build fails (writing nothing) when any gate
trips:

- exact stdout: a record that carries an expected stdout must reproduce it
  byte for byte (instruction, corpus and repair records alike); a record
  without one must at least exit 0;
- environment-dependent corpus items never become records, because their
  golden cannot be reproduced;
- the golden backstop: no record may carry a heap pointer, a raw array dump,
  a host path or an environment dump, and no golden anywhere in the corpus
  manifest may carry host data;
- oracle: a corpus item only trains when its golden has an oracle
  (metadata.oracle python|reviewed); harvested goldens (oracle none) never do.

Each record is stamped with the aether VERSION and binary sha256 it was
verified against. `--report-json` writes the selection counts, the overlap
drops and every gate failure, whether or not the build passes.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any

# Collapse multi-line guard/fx blocks to the compact one-liner form (valid since
# the compiler expands one-liners). Lives alongside this script.
_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
from aether_collapse_oneliners import collapse_text  # noqa: E402
import aether_specialization_corpus_policy as policy  # noqa: E402


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
# Some corpus programs take 24-32 s on a loaded rig; nothing legitimate is
# near a minute.
RUN_TIMEOUT_SECONDS = 60
SAMPLE_NAME = "sample.aether"


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
        # Absolute paths and traversal are host filesystem probes (e.g. a
        # host-awareness example checking "/etc/hosts"), not bundled corpus
        # fixtures — never resolve those against fixtures_dir.
        candidate = pathlib.PurePosixPath(fixture_name)
        if candidate.is_absolute() or ".." in candidate.parts:
            continue
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
    # Only `notes` (behaviour the program demonstrates) reaches the prompt.
    # Curation history lives in metadata.audit_notes and never does.
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
        candidate = pathlib.PurePosixPath(rel_path)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise ValueError(f"refusing to materialize outside sandbox: {rel_path!r}")
        target = root / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")


def normalize_sandbox_paths(text: str, sandbox: pathlib.Path) -> str:
    """Strip the per-run temp directory, so diagnostics read `sample.aether:N:`."""
    if not text:
        return text
    for prefix in {str(sandbox), os.path.realpath(sandbox)}:
        text = text.replace(prefix + os.sep, "").replace(prefix, ".")
    return text


def run_aether(
    *,
    aether_bin: pathlib.Path,
    source: str,
    files: dict[str, str] | None,
    extra_args: tuple[str, ...] = (),
    timeout: int = RUN_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    """Compile and run `source` as sample.aether in a fresh sandbox directory.

    The program is passed by its relative name from inside the sandbox, so
    diagnostics name `sample.aether:N:` rather than a host temp path.
    """
    with tempfile.TemporaryDirectory(prefix="aether-specialize-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        materialize_files(files, tmp_dir)
        (tmp_dir / SAMPLE_NAME).write_text(source, encoding="utf-8")
        argv = [str(aether_bin), "--no-cache", *extra_args, SAMPLE_NAME]
        try:
            proc = subprocess.run(
                argv,
                cwd=str(tmp_dir),
                text=True,
                capture_output=True,
                timeout=timeout,
            )
            returncode, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        except subprocess.TimeoutExpired as exc:
            partial = exc.stdout or ""
            if isinstance(partial, bytes):
                partial = partial.decode("utf-8", errors="replace")
            returncode, stdout = 124, partial
            stderr = f"timeout: no exit within {timeout} s\n"
        return {
            "returncode": returncode,
            "stdout": stdout,
            "stderr": normalize_sandbox_paths(stderr, tmp_dir),
        }


def verify_program(
    *,
    aether_bin: pathlib.Path,
    source: str,
    expected_stdout: str | None,
    files: dict[str, str] | None,
) -> dict[str, Any]:
    run = run_aether(aether_bin=aether_bin, source=source, files=files)
    exact = (
        expected_stdout is not None
        and run["returncode"] == 0
        and run["stdout"] == expected_stdout
    )
    return {
        "returncode": run["returncode"],
        "stdout": run["stdout"],
        "stderr": run["stderr"],
        "exact_stdout_match": exact,
    }


def build_instruction_records(
    payload: dict[str, Any], aether_bin: pathlib.Path, stamp: dict[str, str]
) -> list[dict[str, Any]]:
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
            "aether": stamp,
        }
        records.append(record)
    return records


def build_repair_records(
    payload: dict[str, Any], aether_bin: pathlib.Path, stamp: dict[str, str]
) -> list[dict[str, Any]]:
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
            "aether": stamp,
        }
        records.append(record)
    return records


def corpus_selection_summary(payload: dict[str, Any]) -> dict[str, Any]:
    """Which manifest items train, and why each of the others does not."""
    items = [item for item in payload.get("items", []) if isinstance(item, dict)]
    excluded: collections.Counter[str] = collections.Counter()
    flagged: collections.Counter[str] = collections.Counter()
    selected = 0
    for item in items:
        metadata = policy.item_metadata(item)
        reason = policy.sft_exclusion(item)
        if reason is None:
            selected += 1
        else:
            excluded[reason] += 1
        if metadata.get("environment_dependent"):
            flagged["environment_dependent"] += 1
        if policy.is_harvested(metadata):
            flagged["harvested"] += 1
        if metadata.get("oracle") == "none":
            flagged["oracle_none"] += 1
        if metadata.get("canonical") is False:
            flagged["canonical_false"] += 1
        if metadata.get("include_in_training") is False:
            flagged["include_in_training_false"] += 1
    retired = [
        {key: entry.get(key) for key in ("id", "retired", "reason")}
        for entry in payload.get("retired", [])
        if isinstance(entry, dict)
    ]
    return {
        "manifest_items": len(items),
        "selected": selected,
        "excluded": dict(sorted(excluded.items())),
        "flagged": dict(sorted(flagged.items())),
        "deleted": retired,
    }


def build_corpus_instruction_records(
    payload: dict[str, Any],
    *,
    aether_bin: pathlib.Path,
    fixtures_dir: pathlib.Path,
    stamp: dict[str, str],
    compact: bool = False,
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    compact_collapsed = compact_fallback = 0
    for item in payload.get("items", []):
        repo_path = item.get("repo_path")
        if not isinstance(repo_path, str) or not repo_path:
            continue
        if policy.sft_exclusion(item) is not None:
            continue
        metadata = policy.item_metadata(item)
        expected_stdout = item["stdout"]

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
        verification = None
        if compact:
            # Teach the compact one-liner form. Collapse, then verify the
            # collapsed source reproduces the expected stdout; if it doesn't
            # (an unsupported shape), keep the multi-line original.
            collapsed, n_collapsed = collapse_text(source)
            if n_collapsed > 0:
                v = verify_program(
                    aether_bin=aether_bin,
                    source=collapsed,
                    expected_stdout=expected_stdout,
                    files=files,
                )
                if v.get("exact_stdout_match"):
                    source, verification = collapsed, v
                    compact_collapsed += 1
                else:
                    compact_fallback += 1
                    print(f"  compact fallback (kept multi-line): {source_path.name}")
        if verification is None:
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
            "aether": stamp,
        }
        records.append(record)
    if compact:
        print(
            f"compact_oneliners: collapsed={compact_collapsed} "
            f"fallback_to_multiline={compact_fallback}"
        )
    return records


def load_benchmark_stdout(paths: list[pathlib.Path]) -> set[str]:
    """Collect expected_stdout from benchmark task manifests to de-contaminate training.

    Training on records that reproduce a benchmark's exact output turns the
    benchmark into a memorization check, so prepare_assets drops them by
    default and keeps the board manifests an honest held-out test.
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


def verification_failures(records: list[dict[str, Any]]) -> tuple[list[str], list[str]]:
    """(exit-code failures, exact-stdout mismatches) among built records."""
    rc_failures: list[str] = []
    mismatches: list[str] = []
    for record in records:
        verification = record["verification"]
        if verification["returncode"] != 0:
            rc_failures.append(record["id"])
        elif isinstance(record.get("expected_stdout"), str) and not verification["exact_stdout_match"]:
            mismatches.append(record["id"])
    return rc_failures, mismatches


def record_backstop_hits(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Golden-backstop hits in built records: the golden, the live stdout,
    and (as a last net) anything else the serialized record carries."""
    hits: list[dict[str, Any]] = []
    for record in records:
        for field, text in (
            ("expected_stdout", record.get("expected_stdout")),
            ("verification.stdout", record["verification"].get("stdout")),
            ("record", json.dumps(record, ensure_ascii=True)),
        ):
            names = policy.backstop_hits(text)
            if names:
                hits.append({"id": record["id"], "field": field, "patterns": names})
                break
    return hits


def manifest_host_data_hits(payload: dict[str, Any]) -> list[dict[str, Any]]:
    """Host data in any manifest golden, trained or not (the repo is public)."""
    hits: list[dict[str, Any]] = []
    for item in payload.get("items", []):
        names = policy.backstop_hits(item.get("stdout"), policy.HOST_DATA_PATTERNS)
        if names:
            hits.append({"id": pathlib.Path(str(item.get("repo_path", "?"))).name, "patterns": names})
    return hits


def corpus_oracle_errors(payload: dict[str, Any]) -> list[str]:
    """Selected corpus items whose oracle metadata is missing or inconsistent."""
    errors: list[str] = []
    for item in payload.get("items", []):
        if policy.sft_exclusion(item) is not None:
            continue
        problem = policy.oracle_problem(policy.item_metadata(item))
        if problem:
            errors.append(f"{pathlib.Path(str(item.get('repo_path', '?'))).name}: {problem}")
    return errors


def write_jsonl(path: pathlib.Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record, ensure_ascii=True) + "\n")


def main() -> int:
    global CORPUS_DIR
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
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
        "--compact-oneliners",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="collapse short multi-line guard/fx blocks to the compact one-liner form "
        "(verified per-source; falls back to multi-line if a collapse changes output). "
        "On by default; use --no-compact-oneliners for the verbose form.",
    )
    parser.add_argument(
        "--exclude-benchmark-tasks",
        action="append",
        type=pathlib.Path,
        default=[],
        help="benchmark task JSON whose expected_stdout values are dropped from training "
        "(keeps the benchmark an honest held-out test). Repeatable.",
    )
    parser.add_argument("--report-json", type=pathlib.Path, default=None,
                        help="write selection counts, overlap drops and gate failures here")
    args = parser.parse_args()

    CORPUS_DIR = args.corpus_dir

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")
    stamp = policy.aether_identity(args.aether_bin)

    corpus_payload = read_json(args.corpus_manifest)
    instruction_records = build_instruction_records(
        read_json(args.instruction_manifest), args.aether_bin, stamp
    )
    corpus_instruction_records = build_corpus_instruction_records(
        corpus_payload,
        aether_bin=args.aether_bin,
        fixtures_dir=args.fixtures_dir,
        stamp=stamp,
        compact=args.compact_oneliners,
    )
    instruction_records.extend(corpus_instruction_records)
    repair_records = build_repair_records(read_json(args.repair_manifest), args.aether_bin, stamp)

    exclude_stdout = load_benchmark_stdout(args.exclude_benchmark_tasks)
    instruction_records, dropped_instruction = drop_benchmark_overlap(instruction_records, exclude_stdout)
    repair_records, dropped_repair = drop_benchmark_overlap(repair_records, exclude_stdout)
    if exclude_stdout:
        print(
            f"excluded_benchmark_overlap instruction={len(dropped_instruction)} "
            f"repair={len(dropped_repair)} ids={sorted(dropped_instruction + dropped_repair)}"
        )

    selection = corpus_selection_summary(corpus_payload)
    rc_instruction, mismatch_instruction = verification_failures(instruction_records)
    rc_repair, mismatch_repair = verification_failures(repair_records)
    backstop = record_backstop_hits(instruction_records + repair_records)
    manifest_hits = manifest_host_data_hits(corpus_payload)
    oracle_errors = corpus_oracle_errors(corpus_payload)

    gates = {
        "returncode_failures": {"instruction": rc_instruction, "repair": rc_repair},
        "exact_stdout_mismatches": {"instruction": mismatch_instruction, "repair": mismatch_repair},
        "golden_backstop": {"records": backstop, "manifest_host_data": manifest_hits},
        "oracle_errors": oracle_errors,
    }
    failed = any([rc_instruction, rc_repair, mismatch_instruction, mismatch_repair,
                  backstop, manifest_hits, oracle_errors])

    report = {
        **stamp,
        "status": "failed" if failed else "ok",
        "corpus_selection": selection,
        "benchmark_overlap": {
            "manifests": [policy.display_path(path) for path in args.exclude_benchmark_tasks],
            "dropped": sorted(dropped_instruction + dropped_repair),
        },
        "records": {
            "instruction": len(instruction_records),
            "corpus_instruction": sum(1 for r in instruction_records if r["kind"] == "corpus_instruction_sft"),
            "repair": len(repair_records),
        },
        "gates": gates,
    }
    if args.report_json:
        args.report_json.parent.mkdir(parents=True, exist_ok=True)
        args.report_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(
        f"corpus_selection selected={selection['selected']} "
        + " ".join(f"excluded_{reason}={count}" for reason, count in selection["excluded"].items())
    )
    print(
        "gates "
        f"returncode_failures={len(rc_instruction) + len(rc_repair)} "
        f"exact_stdout_mismatches={len(mismatch_instruction) + len(mismatch_repair)} "
        f"backstop_hits={len(backstop) + len(manifest_hits)} "
        f"oracle_errors={len(oracle_errors)}"
    )
    if failed:
        details = []
        if rc_instruction or rc_repair:
            details.append("nonzero exit: " + ", ".join(rc_instruction + rc_repair))
        if mismatch_instruction or mismatch_repair:
            details.append("exact-stdout mismatch: " + ", ".join(mismatch_instruction + mismatch_repair))
        if backstop:
            details.append("golden backstop: " + ", ".join(
                f"{hit['id']} ({hit['field']}: {'/'.join(hit['patterns'])})" for hit in backstop))
        if manifest_hits:
            details.append("host data in manifest golden: " + ", ".join(
                f"{hit['id']} ({'/'.join(hit['patterns'])})" for hit in manifest_hits))
        if oracle_errors:
            details.append("oracle: " + "; ".join(oracle_errors))
        raise SystemExit("verification failed: " + "; ".join(details))

    write_jsonl(args.instruction_jsonl, instruction_records)
    write_jsonl(args.repair_jsonl, repair_records)

    print(f"instruction_records={len(instruction_records)} -> {args.instruction_jsonl}")
    print(f"repair_records={len(repair_records)} -> {args.repair_jsonl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
