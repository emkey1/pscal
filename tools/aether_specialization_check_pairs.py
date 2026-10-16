#!/usr/bin/env python3
"""Check new training pairs (or new board tasks) before they are merged.

    python3 tools/aether_specialization_check_pairs.py FILE.json [--aether-bin BIN]
            [--against OTHER.json ...] [--threshold 0.5] [--forbid NAME ...]

FILE.json holds either `{"pairs": [...]}` in the seed_instruction_pairs.json shape
(id, prompt, solution, expected_stdout, optional files) or `{"tasks": [...]}` in
the board shape (id, prompt, reference_solution, expected_stdout, optional files).
Every item must:

- compile and run with exit 0, print exactly expected_stdout, and print nothing
  on stderr (a warning is a defect in a teaching example);
- use none of the identifiers models invent for the Aether API (FORBIDDEN, plus
  any --forbid), outside comments and strings;
- not reproduce any board task's expected stdout;
- stay below --threshold 5-gram Jaccard (comments dropped) against every board
  reference solution and against every item of each --against file, so a sealed
  suite and the training data written for the same area cannot overlap;
- have an id unique within the file and unused in seed_instruction_pairs.json
  (pairs) or in any board manifest (tasks).

Prints one line per item and a summary; exits 1 on any failure.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_specialization_build_dataset as bd  # noqa: E402
import aether_specialization_corpus_policy as policy  # noqa: E402

# Names models have invented for the TOON / MStream API (first seen in the
# 2026-10-15 Qwen3.8 pilot and earlier boards). None exists in Aether.
FORBIDDEN = {
    "toon_get_root", "toon_get_key", "toon_get_at", "toon_get_str", "toon_int", "toon_text",
    "toon_bool", "toon_real", "toon_get", "toon_parse_json", "toon_get_array", "toon_get_obj",
    "toon_array_len", "toon_size", "toon_count", "toon_keys", "yyjson_parse_file", "yyjson_parse",
    "json_parse", "mstream_new", "mstream_create", "mstream_write", "mstream_read", "mstream_free",
    "mstream_buffer", "mstream_from_string",
}
_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def code_identifiers(source: str) -> set[str]:
    text = "\n".join(line.split("//", 1)[0] for line in source.splitlines())
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    return set(_IDENT.findall(text))


def load_items(path: pathlib.Path) -> tuple[str, list[dict]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if "pairs" in payload:
        return "pairs", payload["pairs"]
    if "tasks" in payload:
        return "tasks", payload["tasks"]
    raise SystemExit(f"{path}: expected a 'pairs' or 'tasks' list")


def source_of(item: dict) -> str:
    return item.get("solution") or item.get("reference_solution") or ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=pathlib.Path)
    ap.add_argument("--aether-bin", type=pathlib.Path, default=bd.DEFAULT_AETHER_BIN)
    ap.add_argument("--against", type=pathlib.Path, action="append", default=[],
                    help="another pairs/tasks file nothing here may resemble (repeatable)")
    ap.add_argument("--threshold", type=float, default=bd.SIMILARITY_THRESHOLD)
    ap.add_argument("--forbid", action="append", default=[], help="another invented name to reject")
    args = ap.parse_args()

    kind, items = load_items(args.file)
    forbidden = FORBIDDEN | set(args.forbid)
    boards = policy.default_board_manifests()
    board_stdout: dict[str, str] = {}
    references: list[tuple[str, set]] = []
    board_ids: set[str] = set()
    for manifest in boards:
        if manifest.resolve() == args.file.resolve():
            continue
        for task in policy.load_tasks(manifest):
            board_ids.add(str(task.get("id")))
            if isinstance(task.get("expected_stdout"), str):
                board_stdout[task["expected_stdout"]] = f"{manifest.stem}:{task.get('id')}"
            if isinstance(task.get("reference_solution"), str):
                references.append((f"{manifest.stem}:{task.get('id')}", bd.source_shingles(task["reference_solution"])))
    for other in args.against:
        _, others = load_items(other)
        references += [(f"{other.stem}:{o.get('id')}", bd.source_shingles(source_of(o))) for o in others]
    if kind == "pairs":
        seed = json.loads(bd.DEFAULT_INSTRUCTION_MANIFEST.read_text(encoding="utf-8"))["pairs"]
        taken = {p["id"] for p in seed}
    else:
        taken = board_ids

    failures = 0
    seen: set[str] = set()
    for item in items:
        problems: list[str] = []
        item_id = str(item.get("id"))
        if item_id in seen:
            problems.append("duplicate id in file")
        if item_id in taken:
            problems.append("id already used")
        seen.add(item_id)
        source = source_of(item)
        run = bd.run_aether(aether_bin=args.aether_bin, source=source, files=item.get("files"))
        if run["returncode"] != 0:
            problems.append(f"exit {run['returncode']}: {run['stderr'].strip().splitlines()[0][:140] if run['stderr'].strip() else ''}")
        elif run["stdout"] != item.get("expected_stdout"):
            problems.append(f"stdout differs (got {run['stdout'][:80]!r})")
        elif run["stderr"].strip():
            problems.append(f"stderr not empty: {run['stderr'].strip().splitlines()[0][:140]}")
        invented = sorted(code_identifiers(source) & forbidden)
        if invented:
            problems.append(f"invented API names: {invented}")
        if item.get("expected_stdout") in board_stdout:
            problems.append(f"stdout equals board task {board_stdout[item['expected_stdout']]}")
        mine = bd.source_shingles(source)
        best = ("", 0.0)
        for ref_id, ref in references:
            union = len(mine | ref)
            score = len(mine & ref) / union if union else 0.0
            if score > best[1]:
                best = (ref_id, score)
        if best[1] >= args.threshold:
            problems.append(f"too close to {best[0]} (Jaccard {best[1]:.2f})")
        failures += bool(problems)
        print(f"{'FAIL' if problems else 'ok  '} {item_id}  max-sim {best[1]:.2f} ({best[0]})"
              + (f"  -- {'; '.join(problems)}" if problems else ""))
    print(f"{len(items) - failures} of {len(items)} {kind} pass "
          f"(aether {policy.aether_identity(args.aether_bin).get('aether_version', '?')})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
