#!/usr/bin/env python3
"""Export clean idea-miner programs into instruction_sft pairs for the corpus.

Pulls every program that compiled + ran cleanly (including ones fixed by the
repair loop) out of one or more ``aether_idea_miner.py`` sweep reports,
dedupes near-identical intents (idea-miner sweeps routinely have several
models independently rediscover the same "arrays are value-copied" style
lesson), re-verifies each survivor against the current ``aether`` binary
using the same ``verify_program`` the specialization pipeline itself uses,
and writes records in the exact ``instruction_sft`` schema
``aether_specialization_build_dataset.py`` produces so the output can be
concatenated straight into a corpus build's ``aether_instruction_sft.jsonl``.

Benchmark-overlap de-contamination (see ``load_benchmark_stdout`` /
``drop_benchmark_overlap`` in that module) is applied with the same intent:
keep the doc-bench task manifests an honest held-out test.
"""

from __future__ import annotations

import argparse
import itertools
import json
import pathlib
import re
import sys
from typing import Any

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_specialization_build_dataset as bd  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_REPORTS = [
    REPO_ROOT / "Tests" / "aether_doc_bench" / "out" / "idea_miner_unusual" / "openai_unusual.json",
    REPO_ROOT / "Tests" / "aether_doc_bench" / "out" / "idea_miner_unusual" / "gemini_unusual.json",
    REPO_ROOT / "Tests" / "aether_doc_bench" / "out" / "idea_miner_unusual" / "local_unusual.json",
]
DEFAULT_AETHER_BIN = REPO_ROOT / "components" / "aether" / "build-sdl" / "aether"
DEFAULT_OUT = (
    REPO_ROOT / "Tests" / "aether_doc_bench" / "out" / "idea_miner_unusual" / "idea_miner_instruction_sft.jsonl"
)
DEFAULT_BENCHMARK_TASKS = [
    REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks.json",
    REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks_v2_pos.json",
    REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks_hard.json",
    REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks_cs.json",
]

# Fixture-backed builtins we can't safely replay without the original support files.
_FIXTURE_PATTERN = re.compile(r'(?:toon_parse_file|fileexists|filereadall|filereadtext)\s*\(\s*"([^"]+)"')


def final_attempt(program: dict[str, Any]) -> dict[str, Any] | None:
    """The last attempt that actually ran clean (rc == 0), i.e. what the model shipped."""
    for attempt in reversed(program.get("attempts") or []):
        if (attempt.get("run") or {}).get("returncode") == 0:
            return attempt
    return None


def sanitize_id(text: str, limit: int = 40) -> str:
    slug = re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")
    return slug[:limit] or "program"


def to_prompt(intent: str) -> str:
    """Idea-miner intents read as descriptions ("Demonstrates X."); the corpus
    wants imperative instructions ("Write an Aether program that demonstrates X.")."""
    intent = intent.strip().rstrip(".")
    for prefix in ("This program ", "The program "):
        if intent.startswith(prefix):
            body = intent[len(prefix):]
            body = body[0].lower() + body[1:] if body else body
            return f"Write an Aether program that {body}."
    first_word = intent.split(" ", 1)[0]
    if first_word.isalpha() and first_word[:1].isupper() and first_word.endswith("s"):
        body = intent[0].lower() + intent[1:]
        return f"Write an Aether program that {body}."
    return f"Write an Aether program. Task: {intent}."


def load_programs(report_paths: list[pathlib.Path]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for path in report_paths:
        if not path.exists():
            print(f"  (skip, not found) {path}", file=sys.stderr)
            continue
        report = json.loads(path.read_text(encoding="utf-8"))
        sweep = path.stem
        for model in report.get("models", []):
            destination_id = model.get("destination_id", "?")
            for program in model.get("programs", []):
                if not program.get("success"):
                    continue
                attempt = final_attempt(program)
                if attempt is None:
                    continue
                intent = (program.get("intent") or "").strip()
                source = attempt.get("source_code") or ""
                if not intent or not source:
                    continue
                if _FIXTURE_PATTERN.search(source):
                    continue
                out.append(
                    {
                        "sweep": sweep,
                        "destination_id": destination_id,
                        "intent": intent,
                        "source": source,
                        "recorded_stdout": (attempt.get("run") or {}).get("stdout", ""),
                    }
                )
    return out


# Filler words from the "Write an Aether program that ..." instruction phrasing itself,
# stripped before comparing intents so clustering reacts to subject matter, not framing.
_STOPWORDS = frozenset(
    "write an aether program that the a to of is are with using into for and on this "
    "shows show demonstrates demonstrate illustrates illustrate highlights highlight "
    "while when where its it as by via passed passing".split()
)
_WORD_PATTERN = re.compile(r"[a-z][a-z0-9']*")


def concept_tokens(intent: str) -> frozenset[str]:
    words = _WORD_PATTERN.findall(intent.lower())
    return frozenset(w for w in words if w not in _STOPWORDS and len(w) > 2)


def jaccard(a: frozenset[str], b: frozenset[str]) -> float:
    if not a or not b:
        return 0.0
    return len(a & b) / len(a | b)


def dedupe(
    programs: list[dict[str, Any]], threshold: float, max_per_cluster: int
) -> tuple[list[dict[str, Any]], int]:
    """Cluster by token-overlap on the intent's subject-matter words (connected
    components, not pairwise nearest-neighbor) since idea-miner intents describing
    the same underlying lesson are lexically diverse enough that no single pair may
    clear the threshold while a chain of pairs still ties the whole family together
    — e.g. nine independently-phrased "arrays are value-copied across calls" intents
    with only 2 pairs individually similar enough on their own."""
    token_sets = [concept_tokens(p["intent"]) for p in programs]
    parent = list(range(len(programs)))

    def find(x: int) -> int:
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a: int, b: int) -> None:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for i, j in itertools.combinations(range(len(programs)), 2):
        if jaccard(token_sets[i], token_sets[j]) >= threshold:
            union(i, j)

    clusters: dict[int, list[int]] = {}
    for i in range(len(programs)):
        clusters.setdefault(find(i), []).append(i)

    keep_indices: set[int] = set()
    dropped = 0
    for members in clusters.values():
        for i in members[:max_per_cluster]:
            keep_indices.add(i)
        dropped += max(0, len(members) - max_per_cluster)

    kept = [programs[i] for i in range(len(programs)) if i in keep_indices]
    return kept, dropped


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="*", type=pathlib.Path, default=DEFAULT_REPORTS)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    parser.add_argument("--dedup-threshold", type=float, default=0.25)
    parser.add_argument("--max-per-cluster", type=int, default=2)
    parser.add_argument("--benchmark-tasks", nargs="*", type=pathlib.Path, default=DEFAULT_BENCHMARK_TASKS)
    parser.add_argument("--id-prefix", default="idea_miner")
    args = parser.parse_args()

    if not args.aether_bin.exists():
        raise SystemExit(f"aether binary not found: {args.aether_bin}")

    print(f"Loading programs from {len(args.reports)} report(s)...")
    programs = load_programs(args.reports)
    print(f"  {len(programs)} clean programs found (post fixture-filter)")

    kept, deduped_out = dedupe(programs, args.dedup_threshold, args.max_per_cluster)
    print(f"  {deduped_out} dropped as near-duplicate intents (threshold={args.dedup_threshold})")
    print(f"  {len(kept)} candidates remain")

    seen_ids: dict[str, int] = {}
    records: list[dict[str, Any]] = []
    reverify_failed = 0
    for prog in kept:
        verification = bd.verify_program(
            aether_bin=args.aether_bin,
            source=prog["source"],
            expected_stdout=prog["recorded_stdout"],
            files=None,
        )
        if verification["returncode"] != 0:
            reverify_failed += 1
            print(
                f"  (drop, re-verify failed) {prog['sweep']}/{prog['destination_id']}: "
                f"{prog['intent'][:60]!r}",
                file=sys.stderr,
            )
            continue

        base = f"{args.id_prefix}_{prog['sweep']}_{prog['destination_id']}_{sanitize_id(prog['intent'])}"
        seen_ids[base] = seen_ids.get(base, 0) + 1
        record_id = base if seen_ids[base] == 1 else f"{base}_{seen_ids[base]}"

        records.append(
            {
                "kind": "instruction_sft",
                "id": record_id,
                "messages": [
                    {
                        "role": "system",
                        "content": "You generate canonical Aether. When asked for code, output raw Aether source only.",
                    },
                    {"role": "user", "content": to_prompt(prog["intent"])},
                    {"role": "assistant", "content": prog["source"]},
                ],
                "expected_stdout": verification["stdout"],
                "files": {},
                "verification": verification,
            }
        )

    if reverify_failed:
        print(f"  {reverify_failed} dropped: no longer reproduce cleanly against {args.aether_bin}", file=sys.stderr)

    exclude_stdout = bd.load_benchmark_stdout([p for p in args.benchmark_tasks if p.exists()])
    records, benchmark_dropped = bd.drop_benchmark_overlap(records, exclude_stdout)
    if benchmark_dropped:
        shown = ", ".join(benchmark_dropped[:5])
        more = "..." if len(benchmark_dropped) > 5 else ""
        print(f"  {len(benchmark_dropped)} dropped: exact stdout overlaps a benchmark task ({shown}{more})", file=sys.stderr)

    bd.write_jsonl(args.out, records)
    print(f"Wrote {len(records)} instruction_sft records -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
