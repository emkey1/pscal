#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from types import SimpleNamespace


REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "Tools"))

import aether_doc_bench as bench


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the Python-only baseline against the Aether doc benchmark task set.",
    )
    parser.add_argument(
        "--output-json",
        type=Path,
        required=True,
        help="where to write the Python-only benchmark report",
    )
    parser.add_argument(
        "--reference-json",
        type=Path,
        required=True,
        help="existing Aether benchmark JSON to use as the comparison reference",
    )
    parser.add_argument(
        "--command-template",
        required=True,
        help="model command template using {prompt_file}",
    )
    parser.add_argument(
        "--tasks",
        type=Path,
        default=bench.DEFAULT_TASKS,
        help="task manifest to execute",
    )
    parser.add_argument(
        "--request-timeout-seconds",
        type=int,
        default=900,
        help="overall timeout for each model request",
    )
    parser.add_argument(
        "--max-output-tokens",
        type=int,
        default=4000,
        help="reported max output tokens for the destination metadata",
    )
    return parser


def refresh_report(report: dict, out_path: Path) -> None:
    results = report["results"]
    report["updated_at_unix"] = int(time.time())
    report["summary"] = bench.summarize(results)
    report["usage_summary"] = bench.summarize_usage(results)
    report["source_token_summary"] = bench.summarize_source_tokens(results)
    report["final_usage_summary"] = bench.summarize_final_usage(results, "all")
    report["run_ok_final_usage_summary"] = bench.summarize_final_usage(results, "run_ok")
    report["exact_final_usage_summary"] = bench.summarize_final_usage(results, "exact")
    report["final_source_token_summary"] = bench.summarize_final_source_tokens(results, "all")
    report["run_ok_final_source_token_summary"] = bench.summarize_final_source_tokens(results, "run_ok")
    report["exact_final_source_token_summary"] = bench.summarize_final_source_tokens(results, "exact")
    report["failure_patterns"] = bench.summarize_failure_patterns(results)
    bench.write_json_atomic(out_path, report)


def main() -> int:
    args = build_arg_parser().parse_args()

    tasks = bench.load_tasks(args.tasks)
    reference_report = json.loads(args.reference_json.read_text(encoding="utf-8"))
    reference_destination = reference_report["destinations"][0]
    reference_variant = reference_destination["variants"][0]

    destination = bench.Destination(
        destination_id="legacy-cli",
        kind="command",
        model=None,
        command_template=args.command_template,
        request_timeout_seconds=args.request_timeout_seconds,
        max_output_tokens=args.max_output_tokens,
    )
    exec_args = SimpleNamespace(
        repair_attempts=0,
        repair_feedback_limit=1200,
        aether_bin=bench.DEFAULT_AETHER_BIN,
    )

    report = {
        "tasks_file": str(args.tasks),
        "created_at_unix": int(time.time()),
        "updated_at_unix": int(time.time()),
        "benchmark_kind": "python_only_baseline",
        "comparison_reference": {
            "path": str(args.reference_json),
            "destination_id": reference_destination["destination_id"],
            "doc_name": reference_variant["doc_name"],
            "summary": reference_variant["summary"],
            "usage_summary": reference_variant.get("usage_summary"),
            "final_source_token_summary": reference_variant.get("final_source_token_summary"),
            "exact_final_source_token_summary": reference_variant.get("exact_final_source_token_summary"),
        },
        "destination": {
            "destination_id": destination.destination_id,
            "kind": destination.kind,
            "command_template": destination.command_template,
            "request_timeout_seconds": destination.request_timeout_seconds,
            "max_output_tokens": destination.max_output_tokens,
        },
        "results": [],
        "summary": bench.summarize([]),
        "usage_summary": bench.summarize_usage([]),
        "source_token_summary": bench.summarize_source_tokens([]),
        "final_usage_summary": bench.summarize_final_usage([], "all"),
        "run_ok_final_usage_summary": bench.summarize_final_usage([], "run_ok"),
        "exact_final_usage_summary": bench.summarize_final_usage([], "exact"),
        "final_source_token_summary": bench.summarize_final_source_tokens([], "all"),
        "run_ok_final_source_token_summary": bench.summarize_final_source_tokens([], "run_ok"),
        "exact_final_source_token_summary": bench.summarize_final_source_tokens([], "exact"),
        "failure_patterns": bench.summarize_failure_patterns([]),
    }

    refresh_report(report, args.output_json)

    for index, task in enumerate(tasks, start=1):
        print(f"[python-only] start {index}/{len(tasks)} {task.task_id}", flush=True)
        case = bench.execute_case(
            initial_prompt=bench.build_python_prompt(task),
            destination=destination,
            task=task,
            args=exec_args,
            runner="python",
            repair_prompt_builder=lambda **kwargs: bench.build_python_repair_prompt(**kwargs),
        )
        case["task_id"] = task.task_id
        case["task_title"] = task.title
        case["repeat_index"] = 0
        report["results"].append(case)
        refresh_report(report, args.output_json)
        run = case.get("run", {})
        print(
            f"[python-only] done {index}/{len(tasks)} {task.task_id} "
            f"generated={int(bool(case.get('generated_ok')))} "
            f"returncode={run.get('returncode', -1)} "
            f"exact={int(bool(run.get('exact_stdout_match')))}",
            flush=True,
        )

    bench.print_text_summary(
        {
            "tasks_file": report["tasks_file"],
            "summary": {
                "total_cases_per_destination": len(tasks),
                "destination_count": 1,
            },
            "destinations": [
                {
                    "destination_id": destination.destination_id,
                    "type": destination.kind,
                    "model": destination.model,
                    "variants": [
                        {
                            "doc_name": "python-only",
                            "doc_approx_tokens": 0,
                            "summary": report["summary"],
                            "usage_summary": report["usage_summary"],
                            "source_token_summary": report["source_token_summary"],
                            "final_source_token_summary": report["final_source_token_summary"],
                            "exact_final_source_token_summary": report["exact_final_source_token_summary"],
                            "failure_patterns": report["failure_patterns"],
                        }
                    ],
                }
            ],
        }
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
