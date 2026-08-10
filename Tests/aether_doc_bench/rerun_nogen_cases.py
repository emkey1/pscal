#!/usr/bin/env python3
"""Re-run benchmark cases where the model returned nothing, and patch them in.

A no-generation case (timeout, empty reply) is a provider event, not a verdict on
the guide, but it scores identically to a wrong answer. These arrive in bursts —
five consecutive tasks failing together is one outage, not five judgments — so
leaving them in biases whichever variant happened to be in flight at the time.

Scans reports for generated_ok == false, re-runs exactly those (model, suite,
variant, task) combinations, splices the new case records into the original
reports, and recomputes every summary from the patched results list using the
harness's own summary functions.

Dry-run by default; pass --apply to actually spend tokens and rewrite reports.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path("/Users/mke/PBuild")
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
    results = variant["results"]
    variant["summary"] = hb.summarize(results)
    variant["usage_summary"] = hb.summarize_usage(results)
    variant["source_token_summary"] = hb.summarize_source_tokens(results)
    variant["final_usage_summary"] = hb.summarize_final_usage(results, "all")
    variant["run_ok_final_usage_summary"] = hb.summarize_final_usage(results, "run_ok")
    variant["exact_final_usage_summary"] = hb.summarize_final_usage(results, "exact")
    variant["final_source_token_summary"] = hb.summarize_final_source_tokens(results, "all")
    variant["run_ok_final_source_token_summary"] = hb.summarize_final_source_tokens(results, "run_ok")
    variant["exact_final_source_token_summary"] = hb.summarize_final_source_tokens(results, "exact")
    variant["failure_patterns"] = hb.summarize_failure_patterns(results)


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
                    if not result.get("generated_ok") or truncated:
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--apply", action="store_true", help="actually re-run and rewrite reports")
    ap.add_argument("--board", choices=sorted(BOARDS), action="append", default=[])
    ap.add_argument("--include-truncated", action="store_true",
                    help="also re-run cases whose source failed to parse on an unclosed "
                         "construct (provider cut the reply off mid-statement)")
    ap.add_argument("--skip-model", action="append", default=[],
                    help="destination id to leave alone; repeatable")
    ap.add_argument("--only-model", action="append", default=[],
                    help="restrict to these destination ids; repeatable")
    args = ap.parse_args()

    boards = args.board or sorted(BOARDS)
    hb = load_harness() if args.apply else None

    env = dict(os.environ)
    key_file = pathlib.Path.home() / "aic"
    if key_file.exists():
        env["OPENAI_API_KEY"] = key_file.read_text().strip()

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
