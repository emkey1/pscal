#!/usr/bin/env python3
"""Check the Aether guides (and an exported reference corpus) for benchmark
contamination.

Three checks against every board manifest (the decontamination list in
tools/aether_specialization_corpus_policy.py):

1. stdout: every runnable ```aether block in a guide (a whole program with
   `fn main`, or a bare statement fragment run as main's body) is compiled
   and run sandboxed (`--deny net,proc`, empty temp dir, 10 s); a
   block whose stdout equals a board task's expected_stdout is a hit. The
   guide is then handing the model that task's answer. Needs --aether-bin;
   skipped (and reported as skipped) when the binary is absent.
2. identifiers: a board module name (a task's support-module file) or an
   identifier such a module exports, appearing anywhere in the guide. Short
   common words (`cube`, `Base`, `Step`) are not counted; only names of six
   or more characters that contain `_` or an inner capital.
3. reference lines: a line of a board task's reference_solution that appears
   verbatim (whitespace-normalised) in a guide code block and carries one of
   those identifiers.

The same identifier and reference-line checks run over the items of an
exported reference corpus (--reference-corpus aether_reference_corpus.json),
since whatever the guide carries flows into training from there.

Report mode is the default: hits are printed (and written with
--report-json) and the exit status is 0. --strict exits 1 on any hit. The
guide-side renames that clear today's hits belong to the first guide pass
(W1-14 part 4); flip CI to --strict after it lands.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_specialization_corpus_policy as policy  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_DOCS_DIR = REPO_ROOT / "components" / "aether" / "docs"
DEFAULT_GUIDES = (
    "aether_for_llms_medium_contexts.md",
    "aether_for_llms_with_small_contexts.md",
    "aether_for_llms_and_others.md",
)
RUN_TIMEOUT_SECONDS = 10
_IDENT_DECL_RE = re.compile(r"\bexport\s+(?:fn|const|type)\s+([A-Za-z_][A-Za-z0-9_]*)")
_MOD_DECL_RE = re.compile(r"^\s*mod\s+([A-Za-z_][A-Za-z0-9_]*)", re.MULTILINE)


def distinctive(name: str) -> bool:
    """A name specific enough that seeing it in a guide is not a coincidence."""
    return len(name) >= 6 and ("_" in name or any(c.isupper() for c in name[1:]))


def normalise_line(line: str) -> str:
    return " ".join(line.split())


def aether_blocks(text: str) -> list[tuple[int, str]]:
    blocks: list[tuple[int, str]] = []
    current: list[str] | None = None
    start = 0
    for number, line in enumerate(text.split("\n"), 1):
        if current is None and line.strip() == "```aether":
            current, start = [], number
        elif current is not None and line.strip() == "```":
            blocks.append((start, "\n".join(current)))
            current = None
        elif current is not None:
            current.append(line)
    return blocks


def code_lines(text: str) -> list[tuple[int, str]]:
    """(line number, normalised line) for every line inside a fenced block."""
    lines: list[tuple[int, str]] = []
    inside = False
    for number, line in enumerate(text.split("\n"), 1):
        if line.strip().startswith("```"):
            inside = not inside
            continue
        if inside and line.strip():
            lines.append((number, normalise_line(line)))
    return lines


def board_facts(manifests: list[pathlib.Path]) -> dict[str, Any]:
    stdout_to_tasks: dict[str, list[str]] = {}
    identifiers: dict[str, set[str]] = {}
    ref_lines: dict[str, set[str]] = {}
    for manifest in manifests:
        for task in policy.load_tasks(manifest):
            task_id = f"{manifest.stem}:{task.get('id')}"
            expected = task.get("expected_stdout")
            if isinstance(expected, str) and expected:
                stdout_to_tasks.setdefault(expected, []).append(task_id)
            for file_name, content in (task.get("files") or {}).items():
                if file_name.endswith(".json") or not isinstance(content, str):
                    continue
                names = {file_name}
                names.update(_IDENT_DECL_RE.findall(content))
                names.update(_MOD_DECL_RE.findall(content))
                for name in names:
                    if distinctive(name):
                        identifiers.setdefault(name, set()).add(task_id)
            for line in (task.get("reference_solution") or "").splitlines():
                normalised = normalise_line(line)
                if normalised:
                    ref_lines.setdefault(normalised, set()).add(task_id)
    ident_re = (
        re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(sorted(map(re.escape, identifiers), key=len, reverse=True)) + r")(?![A-Za-z0-9_])")
        if identifiers else None
    )
    marked_ref_lines = {
        line: tasks for line, tasks in ref_lines.items() if ident_re is not None and ident_re.search(line)
    }
    return {
        "stdout_to_tasks": stdout_to_tasks,
        "identifiers": identifiers,
        "ident_re": ident_re,
        "ref_lines": marked_ref_lines,
    }


def text_hits(text: str, facts: dict[str, Any]) -> tuple[list[dict], list[dict]]:
    identifier_hits: list[dict] = []
    ident_re = facts["ident_re"]
    if ident_re is not None:
        for number, line in enumerate(text.split("\n"), 1):
            for match in ident_re.finditer(line):
                name = match.group(1)
                identifier_hits.append({
                    "line": number, "identifier": name,
                    "tasks": sorted(facts["identifiers"][name]),
                })
    reference_hits = [
        {"line": number, "text": line, "tasks": sorted(facts["ref_lines"][line])}
        for number, line in code_lines(text)
        if line in facts["ref_lines"]
    ]
    return identifier_hits, reference_hits


def runnable_program(source: str) -> str | None:
    """A guide block as a whole program: as written when it has `fn main`, or
    wrapped as main's body when it is a bare statement fragment. Blocks of
    declarations without a main print nothing and are skipped."""
    if re.search(r"\bfn\s+main\s*\(", source):
        return source
    if re.search(r"^\s*(fn|type|mod|use|const|export|@)", source, re.MULTILINE):
        return None
    if "..." in source:
        return None  # prose elision, not real source
    body = "\n".join("    " + line for line in source.split("\n"))
    return "fn main() -> Void {\n" + body + "\n    ret;\n}\n"


def run_block(aether_bin: pathlib.Path, source: str) -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="aether-guide-contam-") as tmp_name:
        tmp = pathlib.Path(tmp_name)
        (tmp / "block.aether").write_text(source + "\n", encoding="utf-8")
        try:
            proc = subprocess.run(
                [str(aether_bin), "--no-cache", "--deny", "net,proc", "block.aether"],
                cwd=tmp, text=True, capture_output=True, timeout=RUN_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired:
            return 124, ""
        return proc.returncode, proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--docs-dir", type=pathlib.Path, default=DEFAULT_DOCS_DIR,
                        help="directory holding the guides (default: components/aether/docs)")
    parser.add_argument("--guide", action="append", default=None,
                        help="guide file name inside --docs-dir (repeatable; default: medium, small, full)")
    parser.add_argument("--bench-dir", type=pathlib.Path, default=policy.DEFAULT_BENCH_DIR)
    parser.add_argument("--benchmark-tasks", action="append", type=pathlib.Path, default=None,
                        help="board manifest (repeatable; default: every board manifest)")
    parser.add_argument("--aether-bin", type=pathlib.Path, default=REPO_ROOT / "build" / "bin" / "aether",
                        help="binary that runs the guide blocks; the stdout check is skipped without it")
    parser.add_argument("--reference-corpus", type=pathlib.Path, action="append", default=[],
                        help="exported aether_reference_corpus.json to grep as well (repeatable)")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any hit (default: report only)")
    parser.add_argument("--report-json", type=pathlib.Path, default=None)
    args = parser.parse_args()

    manifests = args.benchmark_tasks or policy.default_board_manifests(args.bench_dir)
    facts = board_facts(manifests)
    guides = args.guide or list(DEFAULT_GUIDES)
    run_blocks = args.aether_bin.is_file()

    report: dict[str, Any] = {
        "manifests": [policy.display_path(path) for path in manifests],
        "board_identifiers": sorted(facts["identifiers"]),
        "stdout_check": "ran" if run_blocks else f"skipped: no aether binary at {policy.display_path(args.aether_bin)}",
        "guides": {},
        "reference_corpus": {},
    }
    if run_blocks:
        report.update(policy.aether_identity(args.aether_bin))
    totals = {"stdout": 0, "identifiers": 0, "reference_lines": 0}
    stdout_tasks: set[str] = set()

    for guide in guides:
        path = args.docs_dir / guide
        if not path.is_file():
            raise SystemExit(f"guide not found: {guide} in {policy.display_path(args.docs_dir)}")
        text = path.read_text(encoding="utf-8")
        stdout_hits: list[dict] = []
        runnable = 0
        if run_blocks:
            for line, source in aether_blocks(text):
                program = runnable_program(source)
                if program is None:
                    continue
                runnable += 1
                returncode, stdout = run_block(args.aether_bin, program)
                if returncode == 0 and stdout in facts["stdout_to_tasks"]:
                    stdout_hits.append({"line": line, "tasks": facts["stdout_to_tasks"][stdout]})
        identifier_hits, reference_hits = text_hits(text, facts)
        report["guides"][guide] = {
            "stamp": next(iter(re.findall(r"\*Guide version: ([0-9-]+)\*", text)), None),
            "runnable_blocks": runnable,
            "stdout_hits": stdout_hits,
            "identifier_hits": identifier_hits,
            "reference_line_hits": reference_hits,
        }
        totals["stdout"] += len(stdout_hits)
        for hit in stdout_hits:
            stdout_tasks.update(task.split(":", 1)[1] for task in hit["tasks"])
        totals["identifiers"] += len(identifier_hits)
        totals["reference_lines"] += len(reference_hits)
        print(
            f"{guide}: runnable={runnable} stdout_hits={len(stdout_hits)} "
            f"identifier_hits={len(identifier_hits)} reference_line_hits={len(reference_hits)}"
        )
        for hit in stdout_hits:
            print(f"  stdout  L{hit['line']}: block output equals {', '.join(hit['tasks'])}")
        for hit in identifier_hits:
            print(f"  ident   L{hit['line']}: {hit['identifier']} ({', '.join(hit['tasks'])})")
        for hit in reference_hits:
            print(f"  refline L{hit['line']}: {hit['text']} ({', '.join(hit['tasks'])})")

    for corpus in args.reference_corpus:
        payload = json.loads(corpus.read_text(encoding="utf-8"))
        corpus_report = {}
        for item in payload.get("items", []):
            identifier_hits, reference_hits = text_hits(str(item.get("content", "")), facts)
            title = str(item.get("title") or item.get("path"))
            corpus_report[title] = {
                "identifier_hits": len(identifier_hits),
                "reference_line_hits": len(reference_hits),
            }
            totals["identifiers"] += len(identifier_hits)
            totals["reference_lines"] += len(reference_hits)
            print(
                f"{policy.display_path(corpus)}::{title}: identifier_hits={len(identifier_hits)} "
                f"reference_line_hits={len(reference_hits)}"
            )
        report["reference_corpus"][policy.display_path(corpus)] = corpus_report

    hits = sum(totals.values())
    report["totals"] = totals
    report["stdout_tasks"] = sorted(stdout_tasks)
    report["mode"] = "strict" if args.strict else "report"
    print(
        f"checked {len(manifests)} board manifests, {len(guides)} guides: "
        f"stdout={totals['stdout']} (board tasks reproduced: {len(stdout_tasks)}"
        f"{': ' + ', '.join(sorted(stdout_tasks)) if stdout_tasks else ''}) "
        f"identifiers={totals['identifiers']} "
        f"reference_lines={totals['reference_lines']} ({report['mode']} mode)"
    )
    if args.report_json:
        args.report_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 1 if (args.strict and hits) else 0


if __name__ == "__main__":
    raise SystemExit(main())
