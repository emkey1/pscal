#!/usr/bin/env python3
"""Re-run benchmark cases where the model returned nothing, and patch them in.

A no-generation case (timeout, empty reply) is a provider event, not a verdict on
the guide, but it scores identically to a wrong answer. These arrive in bursts —
five consecutive tasks failing together is one outage, not five judgments — so
leaving them in biases whichever variant happened to be in flight at the time.

Scans reports for cases that measured nothing -- infra_failed (HTTP 4xx/5xx,
402/429/quota, transport, provider deadline, empty reply) or, in reports
written before that tag, generated_ok == false -- re-runs exactly those
(model, suite, variant, task, repeat) combinations, splices the new case
records into the original reports, and recomputes every summary from the
patched results list using the harness's own summary functions. A program
that timed out is a measurement (rc 124, generated_ok true) and is left alone.

Two modes:
  --report GLOB   (repeatable) any harness report. Everything a re-run needs is
                  read from the report itself: tasks file, destinations config,
                  guide path, the binary and its sha256 (so the re-run uses the
                  same binary), --aether-arg, seed base and the repeat index (so
                  repeat r is re-requested with seed base + r). Exits 3 while
                  infra-failed cases remain after --max-rounds.
  --board NAME    the two pre-2026-08-10 boards, as before.

Dry-run by default; pass --apply to actually spend tokens and rewrite reports.
"""
from __future__ import annotations

import argparse
import glob
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
BENCH = ROOT / "Tests" / "aether_doc_bench"
HARNESS = ROOT / "tools" / "aether_doc_bench.py"
AETHER_BIN = ROOT / "build" / "bin" / "aether"

SUITE_MANIFEST = {
    "simple": "tasks_v2_pos.json",
    "large": "tasks_hard_v2.json",
    "cs": "tasks_cs.json",
    "nontoon": "tasks_hard_nontoon.json",
}

# Both boards are pre-2026-08-10 runs and now live under results/history/.
BOARDS = {
    "gemini": ("results/history/guide_full_vs_medium_20260729", "destinations.guided_2026-07-20.gemini.json"),
    "openai": ("results/history/guide_full_vs_medium_openai_20260729", "destinations.guided_2026-07-20.openai.json"),
}


def load_harness():
    spec = importlib.util.spec_from_file_location("aether_doc_bench", HARNESS)
    mod = importlib.util.module_from_spec(spec)
    # Register before exec: @dataclass resolves cls.__module__ through
    # sys.modules, and blows up on a module that isn't there yet.
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def recompute(hb, variant: dict) -> None:
    """Rebuild every summary block on a variant from its (patched) results."""
    hb.refresh_variant_summaries(variant)


_HARNESS_MODULE = None


def _harness():
    """The harness module, loaded once (needs_rerun also runs on dry-run paths)."""
    global _HARNESS_MODULE
    if _HARNESS_MODULE is None:
        _HARNESS_MODULE = load_harness()
    return _HARNESS_MODULE


def needs_rerun(result: dict) -> bool:
    """A case that measured nothing: tagged infra_failed, or (older reports) a
    case whose model call produced nothing. Timeouts keep generated_ok. A
    no_answer attempt (the model generated for its whole budget without a
    program) is a verdict on the model and is never re-run."""
    hb = _harness()
    if hb.case_is_infra_failed(result):
        return True
    attempts = result.get("attempts") or []
    if any(hb.attempt_is_no_answer(a) for a in attempts):
        return False
    return not result.get("generated_ok") and not any(a.get("not_sent") for a in attempts)


def find_nogen(outdir: pathlib.Path, include_truncated: bool = False) -> list[dict]:
    todo = []
    for path in sorted(outdir.glob("*.json")):
        if path.name.endswith(".partial"):
            continue
        suite = next((s for s in SUITE_MANIFEST if path.stem.endswith("_" + s)), None)
        if not suite:
            continue
        model = path.stem[: -(len(suite) + 1)]
        try:
            report = json.loads(path.read_text())
        except json.JSONDecodeError:
            print(f"skip (incomplete JSON): {path.name}", file=sys.stderr)
            continue
        for dest in report.get("destinations", []):
            for variant in dest.get("variants", []):
                for result in variant.get("results", []):
                    # Two distinct provider failures look nothing alike in the
                    # report. A dead request leaves generated_ok False. A reply
                    # the provider cut off mid-statement leaves generated_ok
                    # True with source that cannot parse — the compiler reports
                    # an unclosed construct, which well-formed wrong answers
                    # essentially never produce. Both are infrastructure, not
                    # the model's verdict on the guide.
                    run = result.get("run") or {}
                    err = (run.get("stderr") or "").lower()
                    truncated = (
                        include_truncated
                        and result.get("generated_ok")
                        and not run.get("exact_stdout_match")
                        and ("to close" in err or "unterminated" in err or "unexpected end" in err)
                    )
                    if needs_rerun(result) or truncated:
                        todo.append({
                            "report": path,
                            "model": model,
                            "suite": suite,
                            "variant": variant["doc_name"],
                            "task": result["task_id"],
                        })
    return todo


def rerun_one(item: dict, config: pathlib.Path, env: dict) -> dict | None:
    """Re-run a single case and return its fresh case record."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out = pathlib.Path(tmp.name)
    cmd = [
        sys.executable, str(HARNESS),
        "--tasks", str(BENCH / SUITE_MANIFEST[item["suite"]]),
        "--destinations-config", str(config),
        "--destination", item["model"],
        "--docs", item["variant"],
        "--task", item["task"],
        "--repair-attempts", "2",
        "--aether-bin", str(AETHER_BIN),
        "--output-json", str(out),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        print(f"  ! harness exit {proc.returncode}: {proc.stderr.strip()[:200]}")
    try:
        fresh = json.loads(out.read_text())
    except Exception as exc:
        print(f"  ! unreadable re-run output: {exc}")
        return None
    finally:
        out.unlink(missing_ok=True)

    for dest in fresh.get("destinations", []):
        for variant in dest.get("variants", []):
            for result in variant.get("results", []):
                if result["task_id"] == item["task"]:
                    return result
    return None


def _expand(recorded: str | None) -> pathlib.Path | None:
    """A path as reports record it (repo-relative, ~/..., or absolute)."""
    if not recorded:
        return None
    path = pathlib.Path(recorded).expanduser()
    return path if path.is_absolute() else ROOT / path


def harness_rerun_argv(report: dict, dest_id: str, variant: dict, task_id: str, repeat_index: int,
                       out: pathlib.Path) -> list[str]:
    """The harness command line that re-runs one case exactly as the report ran it."""
    toolchain = report.get("toolchain") or {}
    config = report.get("run_config") or {}
    skew = report.get("skew_guard") or {}
    argv = [
        sys.executable, str(HARNESS),
        "--tasks", str(_expand(report.get("tasks_file"))),
        "--destinations-config", str(_expand(report.get("destinations_config"))),
        "--destination", dest_id,
        "--task", task_id,
        "--start-repeat", str(int(repeat_index or 0)),
        "--repeats", "1",
        "--aether-bin", str(_expand(toolchain.get("aether_bin") or report.get("aether_bin"))),
        "--output-json", str(out),
    ]
    doc_name, doc_path = variant.get("doc_name"), variant.get("doc_path")
    if doc_path:
        argv += ["--doc", f"{doc_name}={_expand(doc_path)}", "--docs", doc_name]
    else:
        argv += ["--docs", doc_name or "none"]
    if report.get("binary_sha256"):
        argv += ["--aether-bin-sha256", report["binary_sha256"]]
    if skew.get("allowed_by_flag"):
        argv.append("--allow-skew")
    root = (report.get("provenance") or {}).get("aether_root")
    if root:
        argv += ["--aether-root", str(_expand(root))]
    if "sandbox_deny" in toolchain:
        argv += ["--sandbox-deny", toolchain.get("sandbox_deny") or ""]
    argv += [f"--aether-arg={arg}" for arg in toolchain.get("aether_args") or []]
    for flag, key in (("--repair-attempts", "repair_attempts"), ("--repair-feedback-limit", "repair_feedback_limit"),
                      ("--repair-source-limit", "repair_source_limit"), ("--seed-base", "seed_base"),
                      ("--context-margin", "context_margin"), ("--min-output-tokens", "min_output_tokens")):
        if config.get(key) is not None:
            argv += [flag, str(config[key])]
    if config.get("allow_unknown_context"):
        argv.append("--allow-unknown-context")
    return argv


def rerun_report(report_path: pathlib.Path, hb, env: dict, apply: bool) -> tuple[int, int]:
    """Re-run every infra-failed case of one report. Returns (found, still failing)."""
    report = json.loads(report_path.read_text())
    todo = []
    for dest in report.get("destinations", []):
        for variant in dest.get("variants", []):
            for index, result in enumerate(variant.get("results", [])):
                if needs_rerun(result):
                    todo.append((dest, variant, index, result))
    print(f"\n=== {report_path}: {len(todo)} case(s) that measured nothing ===")
    for dest, variant, _, result in todo:
        print(f"  {dest['destination_id']:24} {variant['doc_name']:8} {result['task_id']} "
              f"repeat={result.get('repeat_index', 0)} {result.get('infra_kind') or ''}")
    if not apply or not todo:
        return len(todo), len(todo)
    still = 0
    for dest, variant, index, result in todo:
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
            out = pathlib.Path(tmp.name)
        argv = harness_rerun_argv(report, dest["destination_id"], variant, result["task_id"],
                                  result.get("repeat_index", 0), out)
        proc = subprocess.run(argv, capture_output=True, text=True, env=env)
        fresh = None
        try:
            fresh_report = json.loads(out.read_text())
            for fdest in fresh_report.get("destinations", []):
                for fvariant in fdest.get("variants", []):
                    for fresult in fvariant.get("results", []):
                        if fresult["task_id"] == result["task_id"]:
                            fresh = fresult
        except Exception as exc:  # noqa: BLE001
            print(f"  ! unreadable re-run output ({exc}); harness said: {proc.stderr.strip()[:300]}")
        finally:
            out.unlink(missing_ok=True)
        if fresh is None or needs_rerun(fresh):
            still += 1
            print(f"  -> {result['task_id']} still unmeasured; original kept")
            continue
        fresh["rerun_of_infra_failure"] = {
            "infra_kind": result.get("infra_kind"),
            "fingerprint": result.get("failure_fingerprint"),
        }
        fresh["case_sequence"] = result.get("case_sequence")
        variant["results"][index] = fresh
        print(f"  -> {result['task_id']} measured: exact={bool(fresh['run']['exact_stdout_match'])}")
    for dest in report.get("destinations", []):
        for variant in dest.get("variants", []):
            recompute(hb, variant)
    report["patched_no_generation_cases"] = report.get("patched_no_generation_cases", 0) + len(todo) - still
    report["infra_failed"] = hb.report_infra_failed(report)
    report_path.write_text(json.dumps(report, indent=2))
    return len(todo), still


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--apply", action="store_true", help="actually re-run and rewrite reports")
    ap.add_argument("--board", choices=sorted(BOARDS), action="append", default=[])
    ap.add_argument("--report", action="append", default=[], metavar="GLOB",
                    help="re-run the infra-failed cases of these harness reports (repeatable)")
    ap.add_argument("--max-rounds", type=int, default=2,
                    help="--report mode: re-run rounds before giving up (default 2)")
    ap.add_argument("--include-truncated", action="store_true",
                    help="also re-run cases whose source failed to parse on an unclosed "
                         "construct (provider cut the reply off mid-statement)")
    ap.add_argument("--skip-model", action="append", default=[],
                    help="destination id to leave alone; repeatable")
    ap.add_argument("--only-model", action="append", default=[],
                    help="restrict to these destination ids; repeatable")
    args = ap.parse_args()

    env = dict(os.environ)
    # Credentials come from the environment only (see tools/fleet_env.py for
    # the private overlay); this public tool names no key file.

    if args.report:
        hb = load_harness()
        paths = sorted({pathlib.Path(p) for pattern in args.report for p in glob.glob(pattern, recursive=True)})
        if not paths:
            print("no reports matched", file=sys.stderr)
            return 1
        remaining = 0
        for path in paths:
            still = 0
            for _round in range(max(1, args.max_rounds)):
                found, still = rerun_report(path, hb, env, args.apply)
                if not args.apply or still == 0 or found == 0:
                    break
            remaining += still
        if not args.apply:
            print("dry run — pass --apply to re-run and patch")
            return 3 if remaining else 0
        print(f"\n{remaining} case(s) still unmeasured")
        return 3 if remaining else 0

    boards = args.board or sorted(BOARDS)
    hb = load_harness() if args.apply else None

    grand = 0
    for board in boards:
        subdir, config_name = BOARDS[board]
        outdir = ROOT / "Tests" / "aether_doc_bench" / subdir
        config = BENCH / config_name
        todo = find_nogen(outdir, include_truncated=args.include_truncated)
        if args.only_model:
            todo = [t for t in todo if t["model"] in args.only_model]
        if args.skip_model:
            skipped = [t for t in todo if t["model"] in args.skip_model]
            todo = [t for t in todo if t["model"] not in args.skip_model]
            if skipped:
                print(f"skipping {len(skipped)} case(s) on: {', '.join(sorted({t['model'] for t in skipped}))}")
        print(f"\n=== {board}: {len(todo)} no-generation cases ===")
        for item in todo:
            print(f"  {item['model']:24} {item['suite']:8} {item['variant']:7} {item['task']}")
        grand += len(todo)
        if not args.apply:
            continue

        # Group by report so each file is read and written once.
        by_report: dict[pathlib.Path, list[dict]] = {}
        for item in todo:
            by_report.setdefault(item["report"], []).append(item)

        for report_path, items in by_report.items():
            report = json.loads(report_path.read_text())
            changed = 0
            for item in items:
                print(f"  re-running {item['model']}/{item['suite']}/{item['variant']}/{item['task']}")
                fresh = rerun_one(item, config, env)
                if fresh is None:
                    print("    -> no result returned, leaving original in place")
                    continue
                ok = fresh.get("generated_ok")
                exact = bool((fresh.get("run") or {}).get("exact_stdout_match"))
                print(f"    -> generated={ok} exact={exact}")
                fresh["rerun_of_no_generation"] = True
                for dest in report["destinations"]:
                    for variant in dest["variants"]:
                        if variant["doc_name"] != item["variant"]:
                            continue
                        for idx, result in enumerate(variant["results"]):
                            if result["task_id"] == item["task"]:
                                variant["results"][idx] = fresh
                                changed += 1
            if changed:
                for dest in report["destinations"]:
                    for variant in dest["variants"]:
                        recompute(hb, variant)
                report["patched_no_generation_cases"] = report.get("patched_no_generation_cases", 0) + changed
                report_path.write_text(json.dumps(report, indent=2))
                print(f"  patched {changed} case(s) into {report_path.name}")

    print(f"\ntotal: {grand} case(s)")
    if not args.apply:
        print("dry run — pass --apply to re-run and patch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
