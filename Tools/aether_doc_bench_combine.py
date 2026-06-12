#!/usr/bin/env python3
"""Combine one or more Aether benchmark reports into a compact comparison."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any


def read_json(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def pct(numerator: int, denominator: int) -> str:
    if denominator <= 0:
        return "0.0%"
    return f"{(numerator / denominator) * 100.0:.1f}%"


def variant_rows(report_path: pathlib.Path, payload: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for destination in payload.get("destinations", []):
        variants = destination.get("variants", [])
        for variant in variants:
            summary = variant.get("summary", {})
            usage = variant.get("usage_summary", {})
            source = variant.get("final_source_token_summary", {})
            rows.append(
                {
                    "report": report_path.name,
                    "destination_id": destination.get("destination_id", ""),
                    "doc_name": variant.get("doc_name", ""),
                    "doc_approx_tokens": int(variant.get("doc_approx_tokens", 0) or 0),
                    "total_cases": int(summary.get("total_cases", 0) or 0),
                    "generated_ok": int(summary.get("generated_ok", 0) or 0),
                    "run_ok": int(summary.get("run_ok", 0) or 0),
                    "exact_stdout_match": int(summary.get("exact_stdout_match", 0) or 0),
                    "resolved_after_repair": int(summary.get("resolved_after_repair", 0) or 0),
                    "prompt_tokens_total": usage.get("prompt_tokens_total"),
                    "completion_tokens_total": usage.get("completion_tokens_total"),
                    "total_tokens_total": usage.get("total_tokens_total"),
                    "final_source_tokens_total": source.get("source_approx_tokens_total"),
                    "final_source_tokens_avg": source.get("source_approx_tokens_avg"),
                    "failure_patterns": variant.get("failure_patterns", []),
                }
            )
    return rows


def render_markdown(rows: list[dict[str, Any]]) -> str:
    lines = [
        "# Aether Benchmark Comparison",
        "",
        "| Report | Destination | Doc | Doc Tok | Cases | Generated | Run | Exact | Repair | Prompt Tok | Completion Tok | Total Tok | Final Src Avg |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        cases = row["total_cases"]
        lines.append(
            "| "
            + " | ".join(
                [
                    row["report"],
                    row["destination_id"],
                    row["doc_name"],
                    str(row["doc_approx_tokens"]),
                    str(cases),
                    f"{row['generated_ok']} ({pct(row['generated_ok'], cases)})",
                    f"{row['run_ok']} ({pct(row['run_ok'], cases)})",
                    f"{row['exact_stdout_match']} ({pct(row['exact_stdout_match'], cases)})",
                    f"{row['resolved_after_repair']} ({pct(row['resolved_after_repair'], cases)})",
                    str(row["prompt_tokens_total"] if row["prompt_tokens_total"] is not None else "-"),
                    str(row["completion_tokens_total"] if row["completion_tokens_total"] is not None else "-"),
                    str(row["total_tokens_total"] if row["total_tokens_total"] is not None else "-"),
                    str(row["final_source_tokens_avg"] if row["final_source_tokens_avg"] is not None else "-"),
                ]
            )
            + " |"
        )

    lines.append("")
    lines.append("## Failure Highlights")
    lines.append("")
    for row in rows:
        lines.append(f"### {row['report']} / {row['destination_id']} / {row['doc_name']}")
        patterns = row["failure_patterns"][:8]
        if not patterns:
            lines.append("- none")
        else:
            for pattern in patterns:
                task_list = ", ".join(pattern.get("task_ids", []))
                lines.append(f"- {pattern.get('fingerprint')}: {pattern.get('count')} [{task_list}]")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=pathlib.Path, help="benchmark JSON reports")
    parser.add_argument("--output-json", type=pathlib.Path, default=None, help="optional combined JSON output")
    parser.add_argument("--output-md", type=pathlib.Path, default=None, help="optional markdown output")
    args = parser.parse_args()

    rows: list[dict[str, Any]] = []
    for report_path in args.reports:
        rows.extend(variant_rows(report_path, read_json(report_path)))

    rows.sort(key=lambda item: (item["destination_id"], item["doc_name"], item["report"]))
    payload = {"rows": rows}
    markdown = render_markdown(rows)

    if args.output_json:
        args.output_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    if args.output_md:
        args.output_md.write_text(markdown, encoding="utf-8")
    print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
