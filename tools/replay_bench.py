#!/usr/bin/env python3
"""Paired replay: re-grade stored benchmark programs on a new aether binary.

Every report the harness writes keeps each attempt's source_code. Re-running
those programs on another binary -- zero GPU, exact, a few seconds -- shows what
a language or engine change does to real model-written programs: which
attempts flip fail->pass, which flip pass->fail, and how the thesis failure
classes move. It is how every release is measured (plan D3: B_n is paired
replay of B0's first attempts), and it is the regression gate.

  python3 tools/replay_bench.py --aether-bin build/bin/aether
  python3 tools/replay_bench.py --aether-bin B 'harness_out/**/*.json' 'cs-aug20/**/*.json'

Inputs are result-JSON globs (default: the tracked results/**, minus
rejected/). Each case's task is resolved through its report's tasks_file;
when the manifest's version has moved since the report, the task definition
of that version is taken from git history if the grading fields differ.

Old and new are graded against the same task definition: old = the stored
returncode and stdout, new = a fresh run through the harness's own
compile_and_run on a hashed copy of --aether-bin. So a flip is the binary's
doing, not an oracle edit's.

Gate. An attempt that goes pass->fail, or that moves INTO silent_wrong from
another failure class, is a regression unless Tests/aether_doc_bench/
replay_waivers.json waives it: waivers are keyed by the CHANGELOG version
that declared the break plus (task_id, source sha256), with a reason, and
apply only when that version lies after the report's binary and at or before
the replay binary. Any unwaived regression exits 1.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import glob
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_doc_bench as adb  # noqa: E402

ROOT = adb.REPO_ROOT
DEFAULT_RESULTS = ["Tests/aether_doc_bench/results/**/*.json"]
DEFAULT_EXCLUDES = ["**/rejected/**"]
DEFAULT_WAIVERS = ROOT / "Tests" / "aether_doc_bench" / "replay_waivers.json"
GRADING_FIELDS = ("expected_stdout", "files", "generated_files", "cwd", "stdin", "expected_returncode",
                  "timeout_seconds", "sandbox_allow")


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


# --------------------------------------------------------------------------- #
# Task resolution
# --------------------------------------------------------------------------- #
class TaskResolver:
    """Find the task definition a stored case was scored against."""

    def __init__(self) -> None:
        self._current: dict[str, dict[str, Any]] = {}
        self._history: dict[tuple[str, str], dict[str, Any] | None] = {}

    def _manifest_path(self, tasks_file: str) -> pathlib.Path:
        path = pathlib.Path(tasks_file).expanduser()
        if not path.is_absolute():
            path = ROOT / path
        return path

    def _load(self, path: pathlib.Path) -> dict[str, Any]:
        key = str(path)
        if key not in self._current:
            self._current[key] = json.loads(path.read_text(encoding="utf-8"))
        return self._current[key]

    def _historical(self, path: pathlib.Path, version: str) -> dict[str, Any] | None:
        key = (str(path), str(version))
        if key in self._history:
            return self._history[key]
        found = None
        try:
            rel = path.resolve().relative_to(ROOT)
        except ValueError:
            rel = None
        if rel is not None:
            revs = subprocess.run(["git", "-C", str(ROOT), "log", "--format=%H", "--", str(rel)],
                                  capture_output=True, text=True).stdout.split()
            for rev in revs:
                shown = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{rel}"],
                                       capture_output=True, text=True)
                if shown.returncode != 0:
                    continue
                try:
                    data = json.loads(shown.stdout)
                except json.JSONDecodeError:
                    continue
                if str(data.get("version")) == str(version):
                    found = data
                    break
        self._history[key] = found
        return found

    def resolve(self, tasks_file: str | None, tasks_version: Any, task_id: str) -> tuple[dict | None, str]:
        """(raw task dict, basis). basis: current | version_moved_compatible |
        historical | version_unresolved | no_manifest | no_task."""
        if not tasks_file:
            return None, "no_manifest"
        path = self._manifest_path(tasks_file)
        if not path.is_file():
            return None, "no_manifest"
        current = self._load(path)
        current_task = next((t for t in current.get("tasks", []) if t.get("id") == task_id), None)
        if tasks_version is None or str(current.get("version")) == str(tasks_version):
            return current_task, ("current" if current_task else "no_task")
        old = self._historical(path, tasks_version)
        if old is None:
            return current_task, ("version_unresolved" if current_task else "no_task")
        old_task = next((t for t in old.get("tasks", []) if t.get("id") == task_id), None)
        if old_task is None:
            return current_task, ("version_unresolved" if current_task else "no_task")
        if current_task and all(current_task.get(f) == old_task.get(f) for f in GRADING_FIELDS):
            return current_task, "version_moved_compatible"
        return old_task, "historical"


def to_task(raw: dict[str, Any]) -> adb.Task:
    return adb.Task(
        task_id=raw["id"], title=raw.get("title", ""), prompt=raw.get("prompt", ""),
        expected_stdout=raw["expected_stdout"], timeout_seconds=int(raw.get("timeout_seconds", 20)),
        cwd=raw.get("cwd"), files=adb.task_files(raw, "replay"),
        expected_returncode=int(raw.get("expected_returncode", 0)),
        stdin=raw.get("stdin"), sandbox_allow=tuple(str(x) for x in (raw.get("sandbox_allow") or ())),
    )


# --------------------------------------------------------------------------- #
# Waivers
# --------------------------------------------------------------------------- #
def load_waivers(path: pathlib.Path) -> dict[str, Any]:
    if not path.is_file():
        return {"schema": 1, "waivers": {}}
    data = json.loads(path.read_text(encoding="utf-8"))
    problems = validate_waivers(data)
    if problems:
        raise SystemExit(f"{path}: " + "; ".join(problems))
    return data


def validate_waivers(data: Any) -> list[str]:
    problems = []
    if not isinstance(data, dict) or not isinstance(data.get("waivers"), dict):
        return ["expected {\"schema\": 1, \"waivers\": {VERSION: {...}}}"]
    for version, block in data["waivers"].items():
        if adb.version_tuple(version) is None:
            problems.append(f"{version!r} is not a CHANGELOG version (YYYY-MM-DD-N)")
        if not isinstance(block, dict) or not str(block.get("reason") or "").strip():
            problems.append(f"{version}: a waiver block needs a reason")
            continue
        for entry in block.get("entries", []):
            if not entry.get("task_id") or len(str(entry.get("source_sha256") or "")) != 64:
                problems.append(f"{version}: each entry needs task_id and a 64-hex source_sha256")
    return problems


def find_waiver(waivers: dict[str, Any], task_id: str, source_sha: str,
                base_version: str | None, new_version: str | None) -> tuple[str | None, str]:
    """(waiving CHANGELOG version or None, note). A waiver for version V applies
    when base < V <= new (an unknown base is accepted and noted)."""
    new_t = adb.version_tuple(new_version)
    base_t = adb.version_tuple(base_version)
    for version, block in sorted(waivers.get("waivers", {}).items()):
        v_t = adb.version_tuple(version)
        for entry in block.get("entries", []):
            if entry.get("task_id") != task_id or entry.get("source_sha256") != source_sha:
                continue
            if new_t is not None and v_t is not None and v_t > new_t:
                continue
            if base_t is not None and v_t is not None and v_t <= base_t:
                continue
            return version, ("base version unknown" if base_t is None else "")
    return None, ""


# --------------------------------------------------------------------------- #
# Replay
# --------------------------------------------------------------------------- #
def iter_report_paths(patterns: list[str], excludes: list[str]) -> list[pathlib.Path]:
    seen: set[pathlib.Path] = set()
    out = []
    excluded: set[pathlib.Path] = set()
    for pattern in excludes:
        for match in glob.glob(str(ROOT / pattern) if not pathlib.Path(pattern).is_absolute() else pattern, recursive=True):
            excluded.add(pathlib.Path(match).resolve())
    for pattern in patterns:
        full = pattern if pathlib.Path(pattern).expanduser().is_absolute() else str(ROOT / pattern)
        for match in sorted(glob.glob(str(pathlib.Path(full).expanduser()), recursive=True)):
            path = pathlib.Path(match).resolve()
            if path in seen or path in excluded or not path.is_file():
                continue
            if any(path.match(ex) for ex in excludes):
                continue
            seen.add(path)
            out.append(path)
    return out


def stored_attempt_view(attempt: dict[str, Any], task: adb.Task) -> dict[str, Any]:
    """The stored attempt, re-graded against `task` (the same basis as the new run)."""
    run = dict(attempt.get("run") or {})
    run.setdefault("expected_returncode", task.expected_returncode)
    run["exact_stdout_match"] = adb.is_exact(task, run.get("returncode", -1), run.get("stdout", ""))
    return {"generated_ok": True, "source_code": attempt.get("source_code", ""), "run": run}


def replay(args: argparse.Namespace) -> dict[str, Any]:
    paths = iter_report_paths(args.results or DEFAULT_RESULTS, args.exclude if args.exclude else DEFAULT_EXCLUDES)
    resolver = TaskResolver()
    waivers = load_waivers(args.waivers)
    new_version = args.toolchain["language_version"]

    jobs: list[dict[str, Any]] = []
    skipped: dict[str, int] = {}
    for path in paths:
        try:
            report = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            skipped["unreadable_report"] = skipped.get("unreadable_report", 0) + 1
            continue
        if not isinstance(report, dict) or "destinations" not in report:
            continue
        base_version = adb.parse_aether_version(str(report.get("aether_version") or ""))["version"] or None
        for dest in report.get("destinations", []):
            for variant in dest.get("variants", []):
                for case in variant.get("results", []):
                    raw, basis = resolver.resolve(report.get("tasks_file"), report.get("tasks_version"), case.get("task_id"))
                    if raw is None:
                        skipped[basis] = skipped.get(basis, 0) + 1
                        continue
                    task = to_task(raw)
                    for index, attempt in enumerate(case.get("attempts") or []):
                        source = attempt.get("source_code") or ""
                        if not source.strip() or attempt.get("runner", "aether") != "aether":
                            continue
                        jobs.append({
                            "report": adb.display_path(path), "destination": dest.get("destination_id"),
                            "doc": variant.get("doc_name"), "task": task, "basis": basis,
                            "attempt_index": index, "kind": "first" if index == 0 else "repair",
                            "source": source, "source_sha256": sha256_text(source),
                            "stored": stored_attempt_view(attempt, task), "base_version": base_version,
                            "repeat_index": case.get("repeat_index", 0),
                        })

    # Run each distinct (task definition, program) once.
    cache: dict[tuple[str, str], dict[str, Any]] = {}
    unique: dict[tuple[str, str], dict[str, Any]] = {}
    for job in jobs:
        key = (sha256_text(json.dumps({f: getattr(job["task"], f, None) for f in ("task_id",) + GRADING_FIELDS},
                                      sort_keys=True, default=str)), job["source_sha256"])
        job["key"] = key
        unique.setdefault(key, job)
    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {key: pool.submit(adb.compile_and_run, job["task"], job["source"], args) for key, job in unique.items()}
        for key, future in futures.items():
            cache[key] = future.result()
    elapsed = time.time() - started

    tallies: dict[str, dict[str, Any]] = {}
    regressions: list[dict[str, Any]] = []
    waived: list[dict[str, Any]] = []
    flips: list[dict[str, Any]] = []
    for job in jobs:
        new_run = cache[job["key"]]
        new_view = {"generated_ok": True, "source_code": job["source"], "run": new_run}
        old_class = adb.classify_attempt(job["stored"])
        new_class = adb.classify_attempt(new_view)
        old_pass, new_pass = old_class == "pass", new_class == "pass"
        tally = tallies.setdefault(job["kind"], {"attempts": 0, "old_pass": 0, "new_pass": 0, "fail_to_pass": 0,
                                                 "pass_to_fail": 0, "into_silent_wrong": 0, "transitions": {}})
        tally["attempts"] += 1
        tally["old_pass"] += int(old_pass)
        tally["new_pass"] += int(new_pass)
        if old_class != new_class:
            label = f"{old_class}->{new_class}"
            tally["transitions"][label] = tally["transitions"].get(label, 0) + 1
        record = {
            "report": job["report"], "destination": job["destination"], "doc": job["doc"],
            "task_id": job["task"].task_id, "repeat_index": job["repeat_index"], "attempt": job["attempt_index"],
            "kind": job["kind"], "basis": job["basis"], "source_sha256": job["source_sha256"],
            "base_version": job["base_version"], "old_class": old_class, "new_class": new_class,
            "new_returncode": new_run["returncode"],
            "new_stderr_head": (new_run.get("stderr") or "").strip().splitlines()[:1],
        }
        if not old_pass and new_pass:
            tally["fail_to_pass"] += 1
            flips.append(record)
        gated = None
        if old_pass and not new_pass:
            tally["pass_to_fail"] += 1
            gated = "pass->fail"
        elif new_class == "silent_wrong" and old_class not in ("pass", "silent_wrong"):
            tally["into_silent_wrong"] += 1
            gated = "into_silent_wrong"
        if gated is None:
            continue
        record["transition"] = gated
        flips.append(record)
        if args.gate == "first" and job["kind"] != "first":
            continue
        version, note = find_waiver(waivers, record["task_id"], record["source_sha256"], job["base_version"], new_version)
        if version:
            record["waived_by"] = version
            if note:
                record["waiver_note"] = note
            waived.append(record)
        else:
            regressions.append(record)

    return {
        "replay_binary": args.toolchain,
        "reports": [adb.display_path(p) for p in paths],
        "skipped_cases": skipped,
        "attempts_replayed": len(jobs),
        "programs_run": len(unique),
        "run_seconds": round(elapsed, 2),
        "tallies": tallies,
        "waived": waived,
        "unwaived": regressions,
        "flips": flips,
        "gate": args.gate,
    }


def print_summary(result: dict[str, Any]) -> None:
    tool = result["replay_binary"]
    print(f"replay on aether {tool['aether_version']} (sha256 {tool['binary_sha256'][:16]}): "
          f"{len(result['reports'])} report(s), {result['attempts_replayed']} attempts, "
          f"{result['programs_run']} distinct programs in {result['run_seconds']}s")
    if result["skipped_cases"]:
        print(f"  skipped cases: {result['skipped_cases']}")
    for kind in ("first", "repair"):
        t = result["tallies"].get(kind)
        if not t:
            continue
        print(f"  {kind:6} attempts {t['attempts']}: pass {t['old_pass']} -> {t['new_pass']}  "
              f"fail->pass {t['fail_to_pass']}  pass->fail {t['pass_to_fail']}  "
              f"into silent_wrong {t['into_silent_wrong']}")
        for label, count in sorted(t["transitions"].items(), key=lambda kv: (-kv[1], kv[0])):
            print(f"           {label}: {count}")
    print(f"  gated ({result['gate']}): {len(result['waived'])} waived, {len(result['unwaived'])} unwaived")
    for record in result["unwaived"]:
        print(f"  UNWAIVED {record['transition']}: {record['report']} {record['task_id']} "
              f"attempt {record['attempt']} sha256 {record['source_sha256'][:12]} "
              f"{record['old_class']}->{record['new_class']} {record['new_stderr_head']}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", nargs="*", help=f"result-JSON globs, ** allowed (default: {DEFAULT_RESULTS[0]})")
    ap.add_argument("--exclude", action="append", default=[], help=f"globs to skip (default: {DEFAULT_EXCLUDES[0]})")
    ap.add_argument("--aether-bin", type=pathlib.Path, default=adb.DEFAULT_AETHER_BIN)
    ap.add_argument("--aether-arg", action="append", default=[], dest="aether_args", metavar="ARG",
                    help="extra aether flag on every call (--aether-arg=FLAG), e.g. an experiment arm")
    ap.add_argument("--sandbox-deny", default="net,proc")
    ap.add_argument("--waivers", type=pathlib.Path, default=DEFAULT_WAIVERS)
    ap.add_argument("--gate", choices=("first", "all"), default="all",
                    help="which attempts a regression is gated on (default all; first attempts are the headline)")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--report-json", type=pathlib.Path)
    ap.add_argument("--check-waivers", action="store_true", help="validate the waiver file and exit")
    ap.add_argument("--emit-waivers", metavar="VERSION",
                    help="print waiver entries for this run's unwaived regressions under VERSION, to review "
                         "and paste into the waiver file")
    args = ap.parse_args(argv)

    if args.check_waivers:
        data = json.loads(args.waivers.read_text(encoding="utf-8"))
        problems = validate_waivers(data)
        for problem in problems:
            print(f"[waivers] {problem}")
        count = sum(len(b.get("entries", [])) for b in data.get("waivers", {}).values())
        print(f"waivers: {len(data.get('waivers', {}))} version(s), {count} entr(ies), {len(problems)} problem(s)")
        return 1 if problems else 0

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")
    run_dir = pathlib.Path(tempfile.mkdtemp(prefix="aether-replay-"))
    try:
        toolchain = adb.snapshot_aether_binary(args.aether_bin, run_dir)
        args.aether_bin = toolchain.pop("path")
        args.aether_bin_display = toolchain["aether_bin"]
        args.binary_sha256 = toolchain["binary_sha256"]
        toolchain["aether_args"] = list(args.aether_args)
        args.toolchain = toolchain
        result = replay(args)
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)
    print_summary(result)
    if args.report_json:
        args.report_json.write_text(json.dumps(result, indent=2))
    if args.emit_waivers:
        entries = [{"task_id": r["task_id"], "source_sha256": r["source_sha256"], "transition": r["transition"],
                    "note": f"{r['report']} attempt {r['attempt']}: {r['old_class']}->{r['new_class']}"}
                   for r in result["unwaived"]]
        print(json.dumps({args.emit_waivers: {"reason": "FILL IN", "entries": entries}}, indent=2))
    return 1 if result["unwaived"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
