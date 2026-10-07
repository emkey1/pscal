#!/usr/bin/env python3
"""First-attempt failure histogram over aether_doc_bench report JSONs (W1-20).

summarize_failure_patterns reports final state only, and stdout_mismatch is
one bucket, so silent traps never separated and the guides' rule order was
not tied to measured frequency. This tool reads the FIRST attempt of every
case and buckets it by:

  * the first coded diagnostic (the error, not a warning before it), else the
    first `[CODE]` in stderr, else the normalised first stderr line (paths and
    numbers stripped), for coded / uncoded errors;
  * a sub-class of the wrong stdout for silent_wrong: numeric_format
    (`3.500000` vs `3`: a number in another shape), numeric_value (same
    shape, other value), whitespace, case, order, missing_line, extra_line,
    no_output, exit_status (right stdout, wrong status) or value;
  * timeout / signal for crash_hang.

Rows group by (variant, guide stamp, aether VERSION) across destinations, so
two guide stamps on one binary, or one stamp on two binaries, never mix.

  python3 tools/aether_failure_histogram.py REPORT_OR_DIR... [--out-md F] [--out-json F]
  python3 tools/aether_failure_histogram.py results/b0_<date> --out-md results/b0_<date>/failure_histogram.md
  python3 tools/aether_failure_histogram.py REPORTS --doc none --by-construct --examples 2   # what `none` reaches for

`--doc none --by-construct` replaces the never-committed none_fail_detail.py:
it keys each failing `none` first attempt by the idea miner's construct tag
(the unresolved name a SCOPE-001 names, or the code), the triage entry point
for what an unguided model reaches for.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys
from typing import Any, Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_doc_bench as adb  # noqa: E402

_CODE_RE = re.compile(r"\[([A-Z]+-\d{3})\]")
_NUM_RE = re.compile(r"-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?")
# Absolute paths, and file names a program or harness run carries.
_PATH_RE = re.compile(r"(?<![\w.])/(?:[\w.-]+/)*[\w.-]+|\b[\w-]+\.(?:aether|py|json|txt)\b")


def report_files(paths: Iterable[pathlib.Path]) -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for path in paths:
        candidates = sorted(path.rglob("*.json")) if path.is_dir() else [path]
        for candidate in candidates:
            try:
                data = json.loads(candidate.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if isinstance(data, dict) and isinstance(data.get("destinations"), list):
                out.append(candidate)
    return out


def normalise_line(line: str) -> str:
    line = adb.strip_run_dirs(line.strip())
    line = _PATH_RE.sub("<path>", line)
    return _NUM_RE.sub("N", line)[:120]


def first_code(run: dict[str, Any]) -> str:
    diag = adb.primary_error_diagnostic(run.get("diagnostics") or []) or {}
    if diag.get("code"):
        return str(diag["code"])
    stderr = run.get("stderr") or ""
    match = _CODE_RE.search(stderr)
    if match:
        return match.group(1)
    lines = [l for l in stderr.splitlines() if l.strip() and "warning" not in l.lower()]
    if diag.get("message"):
        return "uncoded: " + normalise_line(str(diag["message"]))
    return "uncoded: " + (normalise_line(lines[0]) if lines else "(no stderr)")


def _shape(token: str) -> tuple[bool, int]:
    """(is it written as a Real, how many decimals)."""
    real = "." in token or "e" in token.lower()
    return real, len(token.split(".", 1)[1]) if "." in token else 0


def numeric_difference(exp: list[str], obs: list[str]) -> str | None:
    """numeric_format when the lines match with every number masked and some
    number is written in another shape (`3.500000` for `3`, `2.5` for
    `2.50`); numeric_value when only same-shaped values differ."""
    if len(exp) != len(obs) or any(_NUM_RE.sub("N", a) != _NUM_RE.sub("N", b) for a, b in zip(exp, obs)):
        return None
    pairs = [(x, y) for a, b in zip(exp, obs) for x, y in zip(_NUM_RE.findall(a), _NUM_RE.findall(b)) if x != y]
    if not pairs:
        return None
    return "numeric_format" if any(_shape(x) != _shape(y) for x, y in pairs) else "numeric_value"


def _is_subsequence(small: list[str], big: list[str]) -> bool:
    it = iter(big)
    return all(any(line == other for other in it) for line in small)


def stdout_subclass(expected: str | None, observed: str, rc_ok: bool) -> str:
    if expected is None:
        return "unknown_expected"
    if observed == expected:
        return "exit_status" if not rc_ok else "equal"
    if not observed.strip():
        return "no_output"
    exp, obs = expected.splitlines(), observed.splitlines()
    squash = lambda ls: [" ".join(l.split()) for l in ls if l.strip()]  # noqa: E731
    if squash(exp) == squash(obs):
        return "whitespace"
    if [l.lower() for l in squash(exp)] == [l.lower() for l in squash(obs)]:
        return "case"
    numeric = numeric_difference(exp, obs)
    if numeric:
        return numeric
    if sorted(exp) == sorted(obs):
        return "order"
    if len(obs) < len(exp) and _is_subsequence(obs, exp):
        return "missing_line"
    if len(obs) > len(exp) and _is_subsequence(exp, obs):
        return "extra_line"
    return "value"


class Expectations:
    """expected_stdout by (tasks file, task id), loaded from the manifests the
    reports name (relative to the umbrella, or as given)."""

    def __init__(self, override: list[pathlib.Path]):
        self.override = override
        self.cache: dict[str, dict[str, str]] = {}

    def get(self, tasks_file: str | None, task_id: str) -> str | None:
        paths = list(self.override)
        if tasks_file:
            name = pathlib.Path(tasks_file)
            paths += [name, adb.REPO_ROOT / name, adb.REPO_ROOT / "Tests" / "aether_doc_bench" / name.name]
        for path in paths:
            key = str(path)
            if key not in self.cache:
                try:
                    self.cache[key] = {t.task_id: t.expected_stdout for t in adb.load_tasks(path)}
                except (OSError, ValueError, KeyError, SystemExit):
                    self.cache[key] = {}
            if task_id in self.cache[key]:
                return self.cache[key][task_id]
        return None


def bucket(attempt: dict[str, Any], expected: str | None) -> tuple[str, str]:
    cls = adb.classify_attempt(attempt)
    run = attempt.get("run") or {}
    if cls in ("pass", "infra_failed", "not_sent"):
        return cls, cls
    if cls == "silent_wrong":
        rc_ok = run.get("returncode") == run.get("expected_returncode", 0)
        return cls, "stdout:" + stdout_subclass(expected, run.get("stdout") or "", rc_ok)
    if cls == "crash_hang":
        return cls, "timeout" if run.get("timed_out") else f"signal/rc {run.get('returncode')}"
    return cls, first_code(run)


def construct_tag(attempt: dict[str, Any]) -> str:
    import aether_idea_miner as miner

    finding = miner.analyze_failure(attempt.get("source_code") or "", attempt.get("run") or {})
    if not finding:
        return "(ran: wrong output)"
    if finding.get("is_missing_identifier"):
        return f"missing: {finding['offending_identifier']}"
    if finding.get("code"):
        ident = finding.get("offending_identifier")
        return f"{finding['code']}" + (f" '{ident}'" if ident and finding["code"] == "SYN-001" else "")
    return "uncoded: " + normalise_line(finding.get("message") or finding.get("stderr_head") or "")


def collect(files: list[pathlib.Path], expectations: Expectations, doc: str | None,
            by_construct: bool, examples: int = 0) -> dict[str, Any]:
    groups: dict[tuple[str, str, str], dict[str, Any]] = {}
    for path in files:
        report = json.loads(path.read_text(encoding="utf-8"))
        version = str(report.get("aether_version") or "unknown")
        guides = report.get("guides") or {}
        for dest in report["destinations"]:
            for variant in dest.get("variants") or []:
                name = variant.get("doc_name") or "?"
                if doc and name != doc:
                    continue
                stamp = str((guides.get(name) or {}).get("version") or variant.get("doc_version") or "unknown")
                key = (name, stamp, version)
                g = groups.setdefault(key, {"attempts": 0, "classes": collections.Counter(),
                                            "buckets": collections.Counter(), "tasks": collections.defaultdict(set),
                                            "examples": collections.defaultdict(list),
                                            "destinations": set(), "reports": set()})
                g["destinations"].add(dest.get("destination_id") or "?")
                g["reports"].add(adb.display_path(path) if hasattr(adb, "display_path") else str(path))
                for case in variant.get("results") or []:
                    first = adb.first_attempt_of(case)
                    cls, sub = bucket(first, expectations.get(report.get("tasks_file"), case.get("task_id", "")))
                    if by_construct and cls not in ("pass", "infra_failed", "not_sent"):
                        sub = construct_tag(first)
                    g["attempts"] += 1
                    g["classes"][cls] += 1
                    if cls != "pass":
                        g["buckets"][(cls, sub)] += 1
                        g["tasks"][(cls, sub)].add(case.get("task_id", "?"))
                        if len(g["examples"][(cls, sub)]) < examples:
                            run = first.get("run") or {}
                            g["examples"][(cls, sub)].append({
                                "task_id": case.get("task_id"), "destination": dest.get("destination_id"),
                                "stderr": adb.strip_run_dirs((run.get("stderr") or "").strip())[:400],
                                "source_code": (first.get("source_code") or "")[:4000]})
    rows = []
    for (name, stamp, version), g in sorted(groups.items()):
        rows.append({
            "variant": name, "guide_stamp": stamp, "aether_version": version, "attempts": g["attempts"],
            "destinations": sorted(g["destinations"]), "reports": sorted(map(str, g["reports"])),
            "classes": dict(g["classes"]),
            "buckets": [{"class": cls, "bucket": sub, "count": n, "tasks": sorted(g["tasks"][(cls, sub)]),
                         "examples": g["examples"].get((cls, sub), [])}
                        for (cls, sub), n in sorted(g["buckets"].items(), key=lambda kv: (-kv[1], kv[0]))],
        })
    return {"files": [str(adb.display_path(f)) for f in files], "by_construct": by_construct, "groups": rows}


def render_markdown(result: dict[str, Any], top: int) -> str:
    title = "First-attempt failures by construct" if result["by_construct"] else "First-attempt failure histogram"
    lines = [f"# {title}", "", f"Reports: {len(result['files'])}. Generated by `tools/aether_failure_histogram.py`.", ""]
    for g in result["groups"]:
        n = g["attempts"] or 1
        lines += [f"## {g['variant']} @ {g['guide_stamp']} on aether {g['aether_version']}", "",
                  f"{g['attempts']} first attempts across {len(g['destinations'])} destination(s). Classes: "
                  + ", ".join(f"{k} {v} ({100 * v / n:.1f}%)" for k, v in
                              sorted(g["classes"].items(), key=lambda kv: -kv[1])), "",
                  "| class | bucket | count | % of attempts | tasks |", "|---|---|---:|---:|---|"]
        for b in g["buckets"][:top]:
            tasks = ", ".join(b["tasks"][:6]) + (" ..." if len(b["tasks"]) > 6 else "")
            bucket_text = b["bucket"].replace("|", "\\|")
            lines.append(f"| {b['class']} | {bucket_text} | {b['count']} | {100 * b['count'] / n:.1f} | {tasks} |")
        lines.append("")
        for b in g["buckets"][:top]:
            for ex in b.get("examples") or []:
                lines += [f"<details><summary>{b['bucket']} -- {ex['task_id']} ({ex['destination']})</summary>", "",
                          "```", ex["stderr"] or "(no stderr)", "```", "", "```aether", ex["source_code"], "```",
                          "", "</details>", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", type=pathlib.Path, help="report JSONs or directories of them")
    ap.add_argument("--tasks", action="append", type=pathlib.Path, default=[],
                    help="manifest(s) to take expected stdout from (default: each report's tasks_file)")
    ap.add_argument("--doc", help="only this guide variant (e.g. none, medium)")
    ap.add_argument("--by-construct", action="store_true", help="bucket failures by the miner's construct tag")
    ap.add_argument("--top", type=int, default=25, help="rows per group in the Markdown table")
    ap.add_argument("--examples", type=int, default=0,
                    help="include up to N failing generations (stderr + source) per bucket")
    ap.add_argument("--out-md", type=pathlib.Path)
    ap.add_argument("--out-json", type=pathlib.Path)
    args = ap.parse_args(argv)
    files = report_files(args.paths)
    if not files:
        print("no aether_doc_bench reports found", file=sys.stderr)
        return 1
    result = collect(files, Expectations(args.tasks), args.doc, args.by_construct, args.examples)
    markdown = render_markdown(result, args.top)
    if args.out_md:
        args.out_md.write_text(markdown + "\n", encoding="utf-8")
    if args.out_json:
        args.out_json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not args.out_md:
        print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
