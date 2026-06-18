#!/usr/bin/env python3
"""Benchmark Aether guide variants against code-generation tasks.

This harness compares one or more Aether guide documents by prompting an LLM
to solve the same manifest-defined programming tasks, then compiling/running
the generated source with the local `aether` binary.

Two model adapters are supported:

1. `openai`: calls the Responses API with a configured model.
2. `openai_chat_completions`: calls an OpenAI-compatible chat completions API.
3. `openai_completions`: calls an OpenAI-compatible raw completions API.
2. `command`: runs an external command that reads the prompt from a file.

The benchmark focuses on practical success:
- did the model return code?
- did it compile?
- did it run?
- did stdout match exactly?

The output is a JSON report with per-run details plus an aggregate summary.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap
import time
import email.utils
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_TASKS = REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks.json"
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_DESTINATIONS_CONFIG = REPO_ROOT / "Tests" / "aether_doc_bench" / "destinations.template.json"
DOC_VARIANTS: dict[str, pathlib.Path | None] = {
    "full": REPO_ROOT / "components" / "aether" / "docs" / "aether_for_llms_and_others.md",
    "small": REPO_ROOT / "components" / "aether" / "docs" / "aether_for_llms_with_small_contexts.md",
    "none": None,
}
_DESTINATION_CONTEXT_CACHE: dict[tuple[str, str, str], int | None] = {}
OUTPUT_END_MARKER = "__AETHER_BENCH_END__"


class ProviderTimeoutError(RuntimeError):
    pass


@dataclass
class Task:
    task_id: str
    title: str
    prompt: str
    expected_stdout: str
    timeout_seconds: int = 20
    cwd: str | None = None
    files: dict[str, str] | None = None


@dataclass
class Destination:
    destination_id: str
    kind: str
    model: str | None = None
    base_url: str | None = None
    api_key: str | None = None
    api_key_env: str | None = None
    temperature: float = 0.2
    max_output_tokens: int = 3000
    command_template: str | None = None
    after_each_command: str | None = None
    after_each_timeout_seconds: int = 60
    cooldown_seconds: float = 0.0
    prompt_context_limit: int | None = None
    request_timeout_seconds: int = 120
    request_max_retries: int = 0
    retry_backoff_seconds: float = 2.0
    extra_body: dict | None = None


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def write_json_atomic(path: pathlib.Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    tmp_path.replace(path)


def approx_tokens(text: str) -> int:
    # Coarse but stable enough for comparing prompt-footprint impact.
    return max(1, (len(text) + 3) // 4)


def infer_model_size_billions(model_name: str | None) -> float | None:
    if not model_name:
        return None
    text = model_name.lower()
    match = re.search(r"(\d+(?:\.\d+)?)\s*b\b", text)
    if not match:
        return None
    try:
        return float(match.group(1))
    except ValueError:
        return None


def effective_shared_guide_batch_size(args: argparse.Namespace, destination: Destination) -> int:
    requested = max(1, int(getattr(args, "shared_guide_batch_size", 1)))
    if requested <= 1:
        return 1
    model_size_b = infer_model_size_billions(destination.model)
    if model_size_b is not None and model_size_b <= 8.0:
        return 1
    return requested


def _int_or_none(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return None
        try:
            return int(text)
        except ValueError:
            return None
    return None


def normalize_usage(raw_usage: Any) -> dict[str, Any] | None:
    if not isinstance(raw_usage, dict):
        return None

    prompt_tokens = _int_or_none(raw_usage.get("prompt_tokens"))
    completion_tokens = _int_or_none(raw_usage.get("completion_tokens"))
    total_tokens = _int_or_none(raw_usage.get("total_tokens"))

    if prompt_tokens is None:
        prompt_tokens = _int_or_none(raw_usage.get("input_tokens"))
    if completion_tokens is None:
        completion_tokens = _int_or_none(raw_usage.get("output_tokens"))
    if total_tokens is None:
        total_tokens = _int_or_none(raw_usage.get("total_token_count"))

    usage_metadata = raw_usage.get("usageMetadata")
    if isinstance(usage_metadata, dict):
        if prompt_tokens is None:
            prompt_tokens = _int_or_none(usage_metadata.get("promptTokenCount"))
        if completion_tokens is None:
            completion_tokens = _int_or_none(usage_metadata.get("candidatesTokenCount"))
        if total_tokens is None:
            total_tokens = _int_or_none(usage_metadata.get("totalTokenCount"))

    input_details = raw_usage.get("input_tokens_details")
    output_details = raw_usage.get("output_tokens_details")
    cached_tokens = None
    reasoning_tokens = None
    if isinstance(input_details, dict):
        cached_tokens = _int_or_none(input_details.get("cached_tokens"))
    if isinstance(output_details, dict):
        reasoning_tokens = _int_or_none(output_details.get("reasoning_tokens"))

    if total_tokens is None and prompt_tokens is not None and completion_tokens is not None:
        total_tokens = prompt_tokens + completion_tokens

    if (prompt_tokens is None and completion_tokens is None and total_tokens is None and
            cached_tokens is None and reasoning_tokens is None):
        return None

    return {
        "prompt_tokens": prompt_tokens,
        "completion_tokens": completion_tokens,
        "total_tokens": total_tokens,
        "cached_tokens": cached_tokens,
        "reasoning_tokens": reasoning_tokens,
        "provider_raw": raw_usage,
    }


def summarize_usage(results: list[dict[str, Any]]) -> dict[str, Any]:
    prompt_total = 0
    completion_total = 0
    total_total = 0
    cached_total = 0
    reasoning_total = 0
    counted_prompt = 0
    counted_completion = 0
    counted_total = 0
    counted_cached = 0
    counted_reasoning = 0
    attempts_with_usage = 0
    attempts_total = 0

    for result in results:
        attempts = result.get("attempts") or []
        for attempt in attempts:
            attempts_total += 1
            usage = attempt.get("usage")
            if not isinstance(usage, dict):
                continue
            attempts_with_usage += 1

            value = usage.get("prompt_tokens")
            if isinstance(value, int):
                prompt_total += value
                counted_prompt += 1

            value = usage.get("completion_tokens")
            if isinstance(value, int):
                completion_total += value
                counted_completion += 1

            value = usage.get("total_tokens")
            if isinstance(value, int):
                total_total += value
                counted_total += 1

            value = usage.get("cached_tokens")
            if isinstance(value, int):
                cached_total += value
                counted_cached += 1

            value = usage.get("reasoning_tokens")
            if isinstance(value, int):
                reasoning_total += value
                counted_reasoning += 1

    return {
        "attempts_total": attempts_total,
        "attempts_with_usage": attempts_with_usage,
        "coverage_rate": round(attempts_with_usage / attempts_total, 4) if attempts_total else 0.0,
        "prompt_tokens_total": prompt_total if counted_prompt else None,
        "completion_tokens_total": completion_total if counted_completion else None,
        "total_tokens_total": total_total if counted_total else None,
        "cached_tokens_total": cached_total if counted_cached else None,
        "reasoning_tokens_total": reasoning_total if counted_reasoning else None,
    }


def summarize_source_tokens(results: list[dict[str, Any]]) -> dict[str, Any]:
    total = 0
    count = 0
    for result in results:
        attempts = result.get("attempts") or []
        for attempt in attempts:
            value = attempt.get("source_approx_tokens")
            if isinstance(value, int):
                total += value
                count += 1
    return {
        "attempts_total": count,
        "source_approx_tokens_total": total if count else None,
        "source_approx_tokens_avg": round(total / count, 2) if count else None,
    }


def summarize_final_source_tokens(
    results: list[dict[str, Any]],
    success_filter: str = "all",
) -> dict[str, Any]:
    total = 0
    count = 0

    for result in results:
        run = result.get("run") or {}
        if success_filter == "run_ok" and run.get("returncode", -1) != 0:
            continue
        if success_filter == "exact" and not run.get("exact_stdout_match", False):
            continue

        value = result.get("source_approx_tokens")
        if isinstance(value, int):
            total += value
            count += 1

    return {
        "cases_counted": count,
        "source_approx_tokens_total": total if count else None,
        "source_approx_tokens_avg": round(total / count, 2) if count else None,
        "success_filter": success_filter,
    }


def summarize_final_usage(
    results: list[dict[str, Any]],
    success_filter: str = "all",
) -> dict[str, Any]:
    prompt_total = 0
    completion_total = 0
    total_total = 0
    cached_total = 0
    reasoning_total = 0
    counted_prompt = 0
    counted_completion = 0
    counted_total = 0
    counted_cached = 0
    counted_reasoning = 0
    cases_with_usage = 0
    cases_total = 0

    for result in results:
        run = result.get("run") or {}
        if success_filter == "run_ok" and run.get("returncode", -1) != 0:
            continue
        if success_filter == "exact" and not run.get("exact_stdout_match", False):
            continue

        cases_total += 1
        usage = result.get("usage")
        if not isinstance(usage, dict):
            continue
        cases_with_usage += 1

        value = usage.get("prompt_tokens")
        if isinstance(value, int):
            prompt_total += value
            counted_prompt += 1

        value = usage.get("completion_tokens")
        if isinstance(value, int):
            completion_total += value
            counted_completion += 1

        value = usage.get("total_tokens")
        if isinstance(value, int):
            total_total += value
            counted_total += 1

        value = usage.get("cached_tokens")
        if isinstance(value, int):
            cached_total += value
            counted_cached += 1

        value = usage.get("reasoning_tokens")
        if isinstance(value, int):
            reasoning_total += value
            counted_reasoning += 1

    return {
        "cases_total": cases_total,
        "cases_with_usage": cases_with_usage,
        "coverage_rate": round(cases_with_usage / cases_total, 4) if cases_total else 0.0,
        "prompt_tokens_total": prompt_total if counted_prompt else None,
        "completion_tokens_total": completion_total if counted_completion else None,
        "total_tokens_total": total_total if counted_total else None,
        "cached_tokens_total": cached_total if counted_cached else None,
        "reasoning_tokens_total": reasoning_total if counted_reasoning else None,
        "success_filter": success_filter,
    }


def load_tasks(path: pathlib.Path) -> list[Task]:
    raw = json.loads(read_text(path))
    tasks: list[Task] = []
    for item in raw["tasks"]:
        tasks.append(
            Task(
                task_id=item["id"],
                title=item["title"],
                prompt=item["prompt"].strip(),
                expected_stdout=item["expected_stdout"],
                timeout_seconds=int(item.get("timeout_seconds", 20)),
                cwd=item.get("cwd"),
                files=item.get("files"),
            )
        )
    return tasks


def load_destinations(path: pathlib.Path) -> list[Destination]:
    raw = json.loads(read_text(path))
    items = raw.get("destinations", [])
    destinations: list[Destination] = []
    for item in items:
        destinations.append(
            Destination(
                destination_id=item["id"],
                kind=item["type"],
                model=item.get("model"),
                base_url=item.get("base_url"),
                api_key=item.get("api_key"),
                api_key_env=item.get("api_key_env"),
                temperature=float(item.get("temperature", 0.2)),
                max_output_tokens=int(item.get("max_output_tokens", 3000)),
                command_template=item.get("command_template"),
                after_each_command=item.get("after_each_command"),
                after_each_timeout_seconds=int(item.get("after_each_timeout_seconds", 60)),
                cooldown_seconds=float(item.get("cooldown_seconds", 0.0)),
                prompt_context_limit=(
                    int(item["prompt_context_limit"]) if item.get("prompt_context_limit") is not None else None
                ),
                request_timeout_seconds=int(item.get("request_timeout_seconds", 120)),
                request_max_retries=int(item.get("request_max_retries", 0)),
                retry_backoff_seconds=float(item.get("retry_backoff_seconds", 2.0)),
                extra_body=item.get("extra_body"),
            )
        )
    return destinations


def load_report_json(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(read_text(path))


def iter_report_results(
    report: dict[str, Any],
    destination_filter: set[str] | None = None,
    doc_filter: set[str] | None = None,
):
    for destination in report.get("destinations", []):
        destination_id = destination.get("destination_id", "")
        if destination_filter and destination_id not in destination_filter:
            continue
        for variant in destination.get("variants", []):
            doc_name = variant.get("doc_name", "")
            if doc_filter and doc_name not in doc_filter:
                continue
            for result in variant.get("results", []):
                yield destination_id, doc_name, result


def result_metric_ok(result: dict[str, Any], metric: str) -> bool:
    if metric == "generated":
        return bool(result.get("generated_ok", False))
    run = result.get("run", {})
    if metric == "run":
        return run.get("returncode", -1) == 0
    if metric == "exact":
        return bool(run.get("exact_stdout_match", False))
    raise ValueError(f"unknown metric: {metric}")


def compute_task_bucket_stats(
    report_paths: list[pathlib.Path],
    metric: str,
    destination_filter: set[str] | None = None,
    doc_filter: set[str] | None = None,
) -> dict[str, dict[str, Any]]:
    stats: dict[str, dict[str, Any]] = {}
    for path in report_paths:
        report = load_report_json(path)
        for destination_id, doc_name, result in iter_report_results(report, destination_filter, doc_filter):
            task_id = result.get("task_id", "")
            if not task_id:
                continue
            entry = stats.setdefault(
                task_id,
                {
                    "task_id": task_id,
                    "samples": 0,
                    "successes": 0,
                    "failures": 0,
                    "report_paths": [],
                    "destinations": [],
                    "docs": [],
                },
            )
            entry["samples"] += 1
            ok = result_metric_ok(result, metric)
            if ok:
                entry["successes"] += 1
            else:
                entry["failures"] += 1
            if str(path) not in entry["report_paths"]:
                entry["report_paths"].append(str(path))
            if destination_id and destination_id not in entry["destinations"]:
                entry["destinations"].append(destination_id)
            if doc_name and doc_name not in entry["docs"]:
                entry["docs"].append(doc_name)

    for entry in stats.values():
        samples = entry["samples"]
        entry["success_rate"] = round(entry["successes"] / samples, 4) if samples else 0.0
        entry["failure_rate"] = round(entry["failures"] / samples, 4) if samples else 0.0
    return stats


def classify_task_bucket(
    entry: dict[str, Any],
    failure_threshold: float,
) -> str:
    if entry.get("samples", 0) == 0:
        return "no_data"
    if entry["failure_rate"] >= failure_threshold:
        return "unstable"
    return "stable"


def resolve_docs(names: list[str]) -> list[tuple[str, pathlib.Path | None]]:
    resolved: list[tuple[str, pathlib.Path | None]] = []
    for name in names:
        if name not in DOC_VARIANTS:
            raise SystemExit(f"unknown doc variant '{name}', expected one of: {', '.join(DOC_VARIANTS)}")
        resolved.append((name, DOC_VARIANTS[name]))
    return resolved


def build_guide_block(doc_name: str, doc_text: str) -> str:
    if doc_name == "none":
        return textwrap.dedent(
            """\
            Do not assume you have an Aether reference guide for this task.
            Use only what you already know about the language and infer cautiously.
            """
        ).strip()
    return textwrap.dedent(
        f"""\
        Use the following Aether guide as the ground truth for syntax, supported
        features, and style.

        Guide variant: {doc_name}
        --- BEGIN AETHER GUIDE ---
        {doc_text}
        --- END AETHER GUIDE ---
        """
    ).strip()


def _build_bytelevel_decode_map() -> dict[str, int]:
    bs = (list(range(ord("!"), ord("~") + 1))
          + list(range(ord("¡"), ord("¬") + 1))
          + list(range(ord("®"), ord("ÿ") + 1)))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return {chr(c): b for b, c in zip(bs, cs)}


_BYTELEVEL_DECODE_MAP = _build_bytelevel_decode_map()


def decode_bytelevel_artifacts(text: str) -> str:
    """Some model servers (seen with DeepSeek-Coder under vLLM) return GPT-2
    byte-level-BPE artifacts (space=Ġ, newline=Ċ) instead of decoded
    text, which makes every program fail to parse. Detect a fully byte-level
    encoded string and reverse it back to real bytes. No-op for normal output
    (the other model families return clean text, so this never fires)."""
    if "Ġ" not in text and "Ċ" not in text:
        return text
    table = _BYTELEVEL_DECODE_MAP
    if text and all(ch in table for ch in text):
        try:
            return bytes(table[ch] for ch in text).decode("utf-8", "replace")
        except Exception:
            return text
    return text


def sanitize_code(raw: str) -> str:
    text = decode_bytelevel_artifacts(raw)
    text = strip_reasoning_block(text)
    marker_idx = text.find(OUTPUT_END_MARKER)
    if marker_idx != -1:
        text = text[:marker_idx]
    text = text.strip()
    lines = text.splitlines()
    if lines and lines[0].startswith("```"):
        lines = lines[1:]
    while lines and lines[-1].strip() == "```":
        lines.pop()
    text = "\n".join(lines).strip()
    return text


def build_prompt(doc_name: str, doc_text: str, task: Task) -> str:
    return textwrap.dedent(
        f"""\
        You are writing Aether code.

        {build_guide_block(doc_name, doc_text)}

        Write exactly one complete Aether program that solves the task below.

        Requirements:
        - Return only raw Aether source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - After the full program, output a final line containing exactly `{OUTPUT_END_MARKER}`.
        - Keep the program self-contained unless the task explicitly provides files.
        - The program must compile and run with the local `aether` compiler.
        - The program must print exactly the expected output.

        Task ID: {task.task_id}
        Task Title: {task.title}
        Task:
        {task.prompt}

        Expected stdout:
        {task.expected_stdout}

        Aether source:
        """
    )


def build_python_prompt(task: Task) -> str:
    return textwrap.dedent(
        f"""\
        You are writing Python code.

        Write exactly one complete Python 3 program that solves the task below.

        Requirements:
        - Return only raw Python source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - After the full program, output a final line containing exactly `{OUTPUT_END_MARKER}`.
        - Keep the program self-contained unless the task explicitly provides files.
        - The program must run with the local `python3`.
        - The program must print exactly the expected output.

        Task ID: {task.task_id}
        Task Title: {task.title}
        Task:
        {task.prompt}

        Expected stdout:
        {task.expected_stdout}

        Python source:
        """
    )


def build_batch_prompt(doc_name: str, doc_text: str, tasks: list[Task]) -> str:
    task_sections: list[str] = []
    for task in tasks:
        task_sections.append(
            textwrap.dedent(
                f"""\
                Task ID: {task.task_id}
                Task Title: {task.title}
                Task:
                {task.prompt}

                Expected stdout:
                {task.expected_stdout}
                """
            ).strip()
        )

    tasks_blob = "\n\n--- NEXT TASK ---\n\n".join(task_sections)
    return textwrap.dedent(
        f"""\
        You are writing Aether code.

        {build_guide_block(doc_name, doc_text)}

        Solve every task below.

        Return exactly one JSON object with this shape:
        {{
          "results": [
            {{"task_id": "task-id", "source_code": "full Aether program"}},
            {{"task_id": "task-id-2", "source_code": "full Aether program"}}
          ]
        }}

        Requirements:
        - Return only raw JSON.
        - Do not wrap the answer in Markdown fences.
        - After the final `}}` of the JSON object, output a final line containing exactly `{OUTPUT_END_MARKER}`.
        - Include exactly one result for every task below.
        - Each `source_code` value must be one complete Aether program.
        - Do not explain the code.
        - The program for each task must compile and run with the local `aether` compiler.
        - Each program must print exactly the expected output for its task.

        Tasks:
        {tasks_blob}
        """
    )


def build_repair_prompt(
    doc_name: str,
    doc_text: str,
    task: Task,
    previous_source: str,
    attempt_number: int,
    failure_summary: str,
    observed_stdout: str,
    observed_stderr: str,
) -> str:
    return textwrap.dedent(
        f"""\
        You are repairing a failed Aether program.

        {build_guide_block(doc_name, doc_text)}

        Your previous attempt did not satisfy the benchmark task.
        Return one full corrected Aether program.

        Requirements:
        - Return only raw Aether source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - After the full program, output a final line containing exactly `{OUTPUT_END_MARKER}`.
        - Keep the program self-contained unless the task explicitly provides files.
        - The program must compile and run with the local `aether` compiler.
        - The program must print exactly the expected output.

        Task ID: {task.task_id}
        Task Title: {task.title}
        Task:
        {task.prompt}

        Expected stdout:
        {task.expected_stdout}

        Repair attempt number:
        {attempt_number}

        Failure summary:
        {failure_summary}

        Observed stdout:
        {observed_stdout}

        Observed stderr:
        {observed_stderr}

        Previous source:
        {previous_source}

        Corrected Aether source:
        """
    )


def build_python_repair_prompt(
    task: Task,
    previous_source: str,
    attempt_number: int,
    failure_summary: str,
    observed_stdout: str,
    observed_stderr: str,
) -> str:
    return textwrap.dedent(
        f"""\
        You are repairing a failed Python 3 program.

        Return one full corrected Python 3 program.

        Requirements:
        - Return only raw Python source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - After the full program, output a final line containing exactly `{OUTPUT_END_MARKER}`.
        - Keep the program self-contained unless the task explicitly provides files.
        - The program must run with the local `python3`.
        - The program must print exactly the expected output.

        Task ID: {task.task_id}
        Task Title: {task.title}
        Task:
        {task.prompt}

        Expected stdout:
        {task.expected_stdout}

        Repair attempt number:
        {attempt_number}

        Failure summary:
        {failure_summary}

        Observed stdout:
        {observed_stdout}

        Observed stderr:
        {observed_stderr}

        Previous source:
        {previous_source}

        Corrected Python source:
        """
    )


def resolve_api_key(destination: Destination) -> str | None:
    if destination.api_key is not None:
        return destination.api_key
    if destination.api_key_env:
        return os.environ.get(destination.api_key_env)
    return os.environ.get("OPENAI_API_KEY")


def strip_reasoning_block(text: str) -> str:
    """Drop a leading reasoning block from reasoning models (e.g. Qwen3.5).

    The model emits ``<think>...</think>`` before its answer. With the chat
    template's generation prompt the opening ``<think>`` may already be consumed,
    so the reply can start with the closing tag alone. Remove everything up to and
    including the first ``</think>``. No-op for models that emit no think block.
    """
    idx = text.find("</think>")
    if idx != -1:
        return text[idx + len("</think>"):].lstrip()
    return text


def strip_markdown_fences(text: str) -> str:
    text = strip_reasoning_block(text)
    marker_idx = text.find(OUTPUT_END_MARKER)
    if marker_idx != -1:
        text = text[:marker_idx]
    stripped = text.strip()
    lines = stripped.splitlines()
    if lines and lines[0].startswith("```"):
        lines = lines[1:]
    while lines and lines[-1].strip() == "```":
        lines.pop()
    return "\n".join(lines).strip()


def extract_json_object_text(raw: str) -> str:
    text = strip_markdown_fences(raw)
    try:
        json.loads(text)
        return text
    except Exception:
        pass

    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1 and end > start:
        candidate = text[start:end + 1]
        json.loads(candidate)
        return candidate
    raise ValueError("model output did not contain a valid JSON object")


def parse_batch_sources(raw_text: str, expected_task_ids: list[str]) -> dict[str, str]:
    payload = json.loads(extract_json_object_text(raw_text))
    results: dict[str, str] = {}

    if isinstance(payload, dict) and isinstance(payload.get("results"), list):
        for item in payload["results"]:
            if not isinstance(item, dict):
                continue
            task_id = str(item.get("task_id", "")).strip()
            source_code = item.get("source_code")
            if task_id and isinstance(source_code, str):
                results[task_id] = sanitize_code(source_code)
    elif isinstance(payload, dict):
        for task_id in expected_task_ids:
            source_code = payload.get(task_id)
            if isinstance(source_code, str):
                results[task_id] = sanitize_code(source_code)

    return results


def split_int_total(total: int, count: int) -> list[int]:
    if count <= 0:
        return []
    base = total // count
    remainder = total % count
    return [base + (1 if idx < remainder else 0) for idx in range(count)]


def split_usage_across_tasks(usage: dict[str, Any] | None, count: int) -> list[dict[str, Any] | None]:
    if usage is None or count <= 0:
        return [None for _ in range(max(0, count))]

    split_keys = (
        "prompt_tokens",
        "completion_tokens",
        "total_tokens",
        "cached_tokens",
        "reasoning_tokens",
    )
    allocations = [dict(provider_raw={"shared_batch": True}) for _ in range(count)]

    for key in split_keys:
        value = usage.get(key)
        if isinstance(value, int):
            parts = split_int_total(value, count)
            for idx, part in enumerate(parts):
                allocations[idx][key] = part
        else:
            for idx in range(count):
                allocations[idx][key] = None

    return allocations


def http_json_request(
    url: str,
    body: dict[str, Any],
    api_key: str | None,
    timeout_seconds: int = 120,
    max_retries: int = 0,
    retry_backoff_seconds: float = 2.0,
) -> dict[str, Any]:
    headers = {
        "Content-Type": "application/json",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    attempts = max_retries + 1

    for attempt in range(attempts):
        req = urllib.request.Request(
            url,
            method="POST",
            data=json.dumps(body).encode("utf-8"),
            headers=headers,
        )

        try:
            with urllib.request.urlopen(req, timeout=timeout_seconds) as resp:
                return json.loads(resp.read().decode("utf-8", errors="replace"))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            if attempt < max_retries and should_retry_http_status(exc.code):
                sleep_seconds = compute_retry_delay(exc.headers, retry_backoff_seconds, attempt)
                time.sleep(sleep_seconds)
                continue
            raise RuntimeError(f"HTTP API error {exc.code}: {detail}") from exc
        except TimeoutError as exc:
            if attempt < max_retries:
                time.sleep(compute_backoff_delay(retry_backoff_seconds, attempt))
                continue
            raise RuntimeError(f"HTTP API request timed out after {timeout_seconds} seconds") from exc
        except urllib.error.URLError as exc:
            if attempt < max_retries and is_retryable_url_error(exc):
                time.sleep(compute_backoff_delay(retry_backoff_seconds, attempt))
                continue
            reason = getattr(exc, "reason", exc)
            raise RuntimeError(f"HTTP API request failed: {reason}") from exc

    raise RuntimeError("HTTP API request exhausted retries without returning a response")


def should_retry_http_status(status_code: int) -> bool:
    return status_code in (408, 409, 425, 429, 500, 502, 503, 504)


def compute_backoff_delay(base_seconds: float, attempt: int) -> float:
    return max(0.0, base_seconds) * (2 ** attempt)


def parse_retry_after_seconds(headers: Any) -> float | None:
    if not headers:
        return None
    retry_after = headers.get("Retry-After")
    if not retry_after:
        return None
    retry_after = retry_after.strip()
    if not retry_after:
        return None
    if retry_after.isdigit():
        return float(retry_after)
    parsed = email.utils.parsedate_to_datetime(retry_after)
    if not parsed:
        return None
    return max(0.0, parsed.timestamp() - time.time())


def compute_retry_delay(headers: Any, base_seconds: float, attempt: int) -> float:
    retry_after = parse_retry_after_seconds(headers)
    if retry_after is not None:
        return retry_after
    return compute_backoff_delay(base_seconds, attempt)


def is_retryable_url_error(exc: urllib.error.URLError) -> bool:
    reason = getattr(exc, "reason", None)
    if isinstance(reason, TimeoutError):
        return True
    reason_text = str(reason or exc).lower()
    return any(token in reason_text for token in ("timed out", "timeout", "temporarily unavailable", "connection reset"))


def http_json_get(url: str, api_key: str | None) -> dict[str, Any]:
    headers: dict[str, str] = {}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    req = urllib.request.Request(url, method="GET", headers=headers)

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP API error {exc.code}: {detail}") from exc


def get_destination_context_limit(destination: Destination) -> int | None:
    if destination.prompt_context_limit is not None:
        return destination.prompt_context_limit

    if not destination.model or not destination.base_url:
        return None

    normalized_base_url = destination.base_url.rstrip("/")
    if normalized_base_url not in ("http://127.0.0.1:1215/v1", "http://localhost:1215/v1"):
        return None

    cache_key = (destination.destination_id, destination.model, normalized_base_url)
    if cache_key in _DESTINATION_CONTEXT_CACHE:
        return _DESTINATION_CONTEXT_CACHE[cache_key]

    api_key = resolve_api_key(destination)
    payload = http_json_get(normalized_base_url[:-3] + "/api/v0/models", api_key)
    for item in payload.get("data", []):
        if item.get("id") != destination.model:
            continue
        limit = item.get("loaded_context_length") or item.get("max_context_length")
        value = int(limit) if limit else None
        _DESTINATION_CONTEXT_CACHE[cache_key] = value
        return value

    _DESTINATION_CONTEXT_CACHE[cache_key] = None
    return None


def invoke_openai_responses(prompt: str, destination: Destination) -> dict[str, Any]:
    if not destination.model:
        raise RuntimeError("destination model is required for openai_responses")
    base_url = (destination.base_url or "https://api.openai.com/v1").rstrip("/")
    api_key = resolve_api_key(destination)
    if not api_key:
        raise RuntimeError("an API key is required for openai_responses")

    body = {
        "model": destination.model,
        "input": prompt,
        "reasoning": {"effort": "medium"},
        "text": {"verbosity": "low"},
        "max_output_tokens": destination.max_output_tokens,
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature

    payload = http_json_request(
        f"{base_url}/responses",
        body,
        api_key,
        timeout_seconds=destination.request_timeout_seconds,
        max_retries=destination.request_max_retries,
        retry_backoff_seconds=destination.retry_backoff_seconds,
    )
    output_text = payload.get("output_text", "")
    if not output_text:
        raise RuntimeError("Responses API reply did not contain output_text")

    return {
        "raw_text": output_text,
        "response_id": payload.get("id"),
        "usage": payload.get("usage"),
    }


def flatten_chat_content(content: Any) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for item in content:
            if isinstance(item, dict) and item.get("type") in ("text", "output_text"):
                parts.append(str(item.get("text", "")))
        return "".join(parts)
    return ""


def invoke_openai_chat_completions(prompt: str, destination: Destination) -> dict[str, Any]:
    if not destination.model:
        raise RuntimeError("destination model is required for openai_chat_completions")
    base_url = (destination.base_url or "https://api.openai.com/v1").rstrip("/")
    api_key = resolve_api_key(destination)

    body = {
        "model": destination.model,
        "messages": [
            {"role": "user", "content": prompt},
        ],
        "max_tokens": destination.max_output_tokens,
        "stop": [OUTPUT_END_MARKER],
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature
    if destination.extra_body:
        body.update(destination.extra_body)

    payload = http_json_request(
        f"{base_url}/chat/completions",
        body,
        api_key,
        timeout_seconds=destination.request_timeout_seconds,
        max_retries=destination.request_max_retries,
        retry_backoff_seconds=destination.retry_backoff_seconds,
    )
    choices = payload.get("choices") or []
    if not choices:
        raise RuntimeError("chat completions reply did not contain choices")
    message = choices[0].get("message") or {}
    output_text = flatten_chat_content(message.get("content"))
    if not output_text:
        raise RuntimeError("chat completions reply did not contain message content")

    return {
        "raw_text": output_text,
        "response_id": payload.get("id"),
        "usage": payload.get("usage"),
    }


def invoke_openai_completions(prompt: str, destination: Destination) -> dict[str, Any]:
    if not destination.model:
        raise RuntimeError("destination model is required for openai_completions")
    base_url = (destination.base_url or "https://api.openai.com/v1").rstrip("/")
    api_key = resolve_api_key(destination)

    body = {
        "model": destination.model,
        "prompt": prompt,
        "max_tokens": destination.max_output_tokens,
        "stop": [OUTPUT_END_MARKER],
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature

    payload = http_json_request(
        f"{base_url}/completions",
        body,
        api_key,
        timeout_seconds=destination.request_timeout_seconds,
        max_retries=destination.request_max_retries,
        retry_backoff_seconds=destination.retry_backoff_seconds,
    )
    choices = payload.get("choices") or []
    if not choices:
        raise RuntimeError("completions reply did not contain choices")
    output_text = str(choices[0].get("text", ""))
    if not output_text:
        raise RuntimeError("completions reply did not contain text")

    return {
        "raw_text": output_text,
        "response_id": payload.get("id"),
        "usage": payload.get("usage"),
    }


def invoke_command(
    prompt: str,
    command_template: str,
    cwd: pathlib.Path,
    timeout_seconds: int,
) -> dict[str, Any]:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".prompt", delete=False) as handle:
        handle.write(prompt)
        prompt_file = pathlib.Path(handle.name)

    try:
        command = command_template.format(prompt_file=str(prompt_file))
        proc = subprocess.run(
            command,
            shell=True,
            cwd=str(cwd),
            text=True,
            errors="replace",
            capture_output=True,
            timeout=timeout_seconds,
        )
    finally:
        prompt_file.unlink(missing_ok=True)

    if proc.returncode != 0:
        raise RuntimeError(
            "command provider failed with exit code "
            f"{proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )

    stdout_text = proc.stdout
    try:
        payload = json.loads(stdout_text)
    except json.JSONDecodeError:
        payload = None

    if isinstance(payload, dict) and isinstance(payload.get("raw_text"), str):
        result = {
            "raw_text": payload["raw_text"],
            "stderr": payload.get("stderr", proc.stderr),
        }
        usage = normalize_usage(payload.get("usage"))
        if usage is not None:
            result["usage"] = usage
        if payload.get("response_id") is not None:
            result["response_id"] = payload.get("response_id")
        if payload.get("model") is not None:
            result["model"] = payload.get("model")
        return result

    return {
        "raw_text": stdout_text,
        "stderr": proc.stderr,
    }


def run_destination_cleanup(destination: Destination, task: Task, doc_name: str, repeat_index: int) -> None:
    if destination.after_each_command:
        command = destination.after_each_command.format(
            destination_id=destination.destination_id,
            model=destination.model or "",
            task_id=task.task_id,
            doc_name=doc_name,
            repeat_index=repeat_index,
        )
        proc = subprocess.run(
            command,
            shell=True,
            cwd=str(REPO_ROOT),
            text=True,
            errors="replace",
            capture_output=True,
            timeout=destination.after_each_timeout_seconds,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                "after_each_command failed with exit code "
                f"{proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
    if destination.cooldown_seconds > 0:
        time.sleep(destination.cooldown_seconds)


def run_model(prompt: str, destination: Destination) -> dict[str, Any]:
    if destination.kind == "openai_responses":
        return invoke_openai_responses(prompt=prompt, destination=destination)
    if destination.kind == "openai_chat_completions":
        return invoke_openai_chat_completions(prompt=prompt, destination=destination)
    if destination.kind == "openai_completions":
        return invoke_openai_completions(prompt=prompt, destination=destination)
    if destination.kind == "command":
        if not destination.command_template:
            raise RuntimeError("command_template is required for command destinations")
        return invoke_command(
            prompt=prompt,
            command_template=destination.command_template,
            cwd=REPO_ROOT,
            timeout_seconds=max(30, int(destination.request_timeout_seconds)),
        )
    raise RuntimeError(f"unsupported destination type {destination.kind}")


def _run_model_worker(prompt: str, destination: Destination, queue: Any) -> None:
    try:
        queue.put(("ok", run_model(prompt, destination)))
    except Exception as exc:
        queue.put(("err", str(exc)))


def run_model_with_deadline(prompt: str, destination: Destination) -> dict[str, Any]:
    deadline = max(1, int(destination.request_timeout_seconds))
    ctx = multiprocessing.get_context("spawn")
    queue: Any = ctx.Queue()
    proc = ctx.Process(target=_run_model_worker, args=(prompt, destination, queue))
    proc.start()
    proc.join(deadline)
    if proc.is_alive():
        proc.terminate()
        proc.join(5)
        raise ProviderTimeoutError(f"provider request exceeded {deadline} seconds")
    if proc.exitcode not in (0, None) and queue.empty():
        raise RuntimeError(f"provider worker exited unexpectedly with code {proc.exitcode}")
    if queue.empty():
        raise RuntimeError("provider worker returned no result")
    status, payload = queue.get()
    if status == "ok":
        return payload
    raise RuntimeError(payload)


def materialize_task_files(task: Task, work_dir: pathlib.Path) -> None:
    if not task.files:
        return
    for rel_path, content in task.files.items():
        target = work_dir / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")


def compile_and_run(task: Task, source_code: str, args: argparse.Namespace) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="aether-doc-bench-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        program_path = tmp_dir / f"{task.task_id}.aether"
        work_dir = tmp_dir if task.cwd is None else tmp_dir / task.cwd
        work_dir.mkdir(parents=True, exist_ok=True)
        materialize_task_files(task, tmp_dir)
        program_path.write_text(source_code, encoding="utf-8")

        cmd = [str(args.aether_bin), "--no-cache", str(program_path)]
        started = time.time()
        proc = subprocess.run(
            cmd,
            cwd=str(work_dir),
            text=True,
            errors="replace",
            capture_output=True,
            timeout=task.timeout_seconds,
        )
        elapsed = time.time() - started

        stdout = proc.stdout
        stderr = proc.stderr
        exact_match = proc.returncode == 0 and stdout == task.expected_stdout
        diagnostics = None

        if proc.returncode != 0:
            diag_cmd = [str(args.aether_bin), "--diagnostics-json", "--no-cache", str(program_path)]
            diag_proc = subprocess.run(
                diag_cmd,
                cwd=str(work_dir),
                text=True,
                errors="replace",
                capture_output=True,
                timeout=task.timeout_seconds,
            )
            diag_text = (diag_proc.stderr or "").strip()
            if diag_text:
                try:
                    diagnostics = json.loads(diag_text)
                except json.JSONDecodeError:
                    diagnostics = None

        return {
            "command": cmd,
            "returncode": proc.returncode,
            "stdout": stdout,
            "stderr": stderr,
            "diagnostics": diagnostics,
            "elapsed_seconds": round(elapsed, 3),
            "exact_stdout_match": exact_match,
        }


def run_python_task(task: Task, source_code: str) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="python-doc-bench-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        program_path = tmp_dir / f"{task.task_id}.py"
        work_dir = tmp_dir if task.cwd is None else tmp_dir / task.cwd
        work_dir.mkdir(parents=True, exist_ok=True)
        materialize_task_files(task, tmp_dir)
        program_path.write_text(source_code, encoding="utf-8")

        cmd = ["python3", str(program_path)]
        started = time.time()
        proc = subprocess.run(
            cmd,
            cwd=str(work_dir),
            text=True,
            errors="replace",
            capture_output=True,
            timeout=task.timeout_seconds,
        )
        elapsed = time.time() - started

        stdout = proc.stdout
        stderr = proc.stderr
        exact_match = proc.returncode == 0 and stdout == task.expected_stdout

        return {
            "command": cmd,
            "returncode": proc.returncode,
            "stdout": stdout,
            "stderr": stderr,
            "diagnostics": None,
            "elapsed_seconds": round(elapsed, 3),
            "exact_stdout_match": exact_match,
        }


def build_attempt_from_source(
    *,
    task: Task,
    args: argparse.Namespace,
    runner: str,
    source_code: str,
    prompt_kind: str,
    prompt_approx_tokens: int,
    usage: dict[str, Any] | None = None,
    generation_meta: dict[str, Any] | None = None,
    generated_ok_override: bool | None = None,
    generation_error: str | None = None,
    shared_prompt_approx_tokens: int | None = None,
    batch_id: str | None = None,
) -> dict[str, Any]:
    attempt: dict[str, Any] = {
        "prompt_kind": prompt_kind,
        "prompt_approx_tokens": prompt_approx_tokens,
        "runner": runner,
        "usage": usage,
        "generation": generation_meta or {},
        "source_code": source_code,
        "source_approx_tokens": approx_tokens(source_code) if source_code.strip() else 0,
    }
    if shared_prompt_approx_tokens is not None:
        attempt["shared_prompt_approx_tokens"] = shared_prompt_approx_tokens
    if batch_id is not None:
        attempt["batch_id"] = batch_id

    generated_ok = generated_ok_override if generated_ok_override is not None else bool(source_code.strip())
    attempt["generated_ok"] = bool(generated_ok)
    if generation_error:
        attempt["generation_error"] = generation_error

    if attempt["generated_ok"]:
        if runner == "python":
            attempt["run"] = run_python_task(task, source_code)
        else:
            attempt["run"] = compile_and_run(task, source_code, args)
    else:
        attempt["run"] = {
            "returncode": -1,
            "stdout": "",
            "stderr": generation_error or "empty model output",
            "elapsed_seconds": 0.0,
            "exact_stdout_match": False,
        }
    return attempt


def truncate_for_prompt(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[:limit] + "\n...[truncated]..."


def derive_failure_summary(generated_ok: bool, run: dict[str, Any], generation_error: str | None = None) -> str:
    if not generated_ok:
        if generation_error:
            first_line = generation_error.strip().splitlines()[0]
            return f"generation_error: {first_line}"
        return "generation_error: empty model output"
    if run["returncode"] != 0:
        diagnostics = run.get("diagnostics") or []
        if diagnostics:
            first = diagnostics[0] or {}
            code = first.get("code")
            message = (first.get("message") or "").strip()
            if code and message:
                return f"{code}: {message}"
            if code:
                return code
            if message:
                return message
        stderr = (run.get("stderr") or "").strip()
        if stderr:
            return stderr.splitlines()[0]
        return f"nonzero_exit:{run['returncode']}"
    return "stdout_mismatch"


def derive_failure_fingerprint(result: dict[str, Any]) -> str:
    if not result.get("generated_ok", False):
        err = result.get("generation_error", "empty model output")
        return "generation:" + err.strip().splitlines()[0][:160]
    run = result["run"]
    if run["returncode"] != 0:
        diagnostics = run.get("diagnostics") or []
        if diagnostics:
            first = diagnostics[0] or {}
            code = first.get("code")
            phase = first.get("phase") or "unknown"
            kind = first.get("kind") or "unknown"
            message = (first.get("message") or "").strip()
            if code:
                return f"run_error_code:{code}"
            if message:
                return f"run_error_diag:{phase}:{kind}:{message[:160]}"
        stderr = (run.get("stderr") or "").strip()
        if stderr:
            line = stderr.splitlines()[0]
            if ": " in line:
                line = line.split(": ", 1)[1]
            return "run_error:" + line[:160]
        return f"run_error:returncode={run['returncode']}"
    return "stdout_mismatch"


def evaluate_attempt(
    prompt: str,
    prompt_kind: str,
    destination: Destination,
    task: Task,
    args: argparse.Namespace,
    runner: str = "aether",
) -> dict[str, Any]:
    prompt_tokens = approx_tokens(prompt)
    attempt: dict[str, Any] = {
        "prompt_kind": prompt_kind,
        "prompt_approx_tokens": prompt_tokens,
        "runner": runner,
    }
    context_limit = get_destination_context_limit(destination)
    if context_limit is not None and prompt_tokens >= context_limit:
        raise RuntimeError(
            "prompt_too_large: approx prompt tokens "
            f"{prompt_tokens} exceed loaded context {context_limit} for model {destination.model}"
        )
    generation = run_model_with_deadline(prompt, destination)
    source_code = sanitize_code(generation["raw_text"])
    attempt["generation"] = generation
    attempt["usage"] = normalize_usage(generation.get("usage"))
    attempt["generated_ok"] = bool(source_code.strip())
    attempt["source_code"] = source_code
    attempt["source_approx_tokens"] = approx_tokens(source_code) if source_code.strip() else 0
    if attempt["generated_ok"]:
        if runner == "python":
            attempt["run"] = run_python_task(task, source_code)
        else:
            attempt["run"] = compile_and_run(task, source_code, args)
    else:
        attempt["run"] = {
            "returncode": -1,
            "stdout": "",
            "stderr": "empty model output",
            "elapsed_seconds": 0.0,
            "exact_stdout_match": False,
        }
    return attempt


def make_prompt_too_large_record(
    prompt: str,
    context_limit: int,
    destination: Destination,
) -> dict[str, Any]:
    message = (
        "prompt_too_large: approx prompt tokens "
        f"{approx_tokens(prompt)} exceed loaded context {context_limit} for model {destination.model}"
    )
    return {
        "prompt_kind": "initial",
        "prompt_approx_tokens": approx_tokens(prompt),
        "generated_ok": False,
        "generation_error": message,
        "run": {
            "returncode": -1,
            "stdout": "",
            "stderr": message,
            "elapsed_seconds": 0.0,
            "exact_stdout_match": False,
        },
        "attempt_count": 0,
        "resolved_after_repair": False,
        "failure_fingerprint": "generation:" + message,
    }


def summarize(results: list[dict[str, Any]]) -> dict[str, Any]:
    total = len(results)
    generated = sum(1 for item in results if item["generated_ok"])
    compiled_and_ran = sum(1 for item in results if item["run"]["returncode"] == 0)
    exact = sum(1 for item in results if item["run"]["exact_stdout_match"])
    repaired = sum(1 for item in results if item.get("resolved_after_repair", False))
    return {
        "total_cases": total,
        "generated_ok": generated,
        "run_ok": compiled_and_ran,
        "exact_stdout_match": exact,
        "resolved_after_repair": repaired,
        "generated_rate": round(generated / total, 4) if total else 0.0,
        "run_rate": round(compiled_and_ran / total, 4) if total else 0.0,
        "exact_match_rate": round(exact / total, 4) if total else 0.0,
        "repair_recovery_rate": round(repaired / total, 4) if total else 0.0,
    }


def summarize_failure_patterns(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    counts: dict[str, dict[str, Any]] = {}
    for result in results:
        if result["run"]["exact_stdout_match"]:
            continue
        fingerprint = result.get("failure_fingerprint") or derive_failure_fingerprint(result)
        entry = counts.setdefault(
            fingerprint,
            {
                "fingerprint": fingerprint,
                "count": 0,
                "task_ids": [],
            },
        )
        entry["count"] += 1
        if result["task_id"] not in entry["task_ids"]:
            entry["task_ids"].append(result["task_id"])
    return sorted(counts.values(), key=lambda item: (-item["count"], item["fingerprint"]))


def print_text_summary(report: dict[str, Any]) -> None:
    print(f"tasks file    : {report['tasks_file']}")
    print(f"cases/dest    : {report['summary']['total_cases_per_destination']}")
    print(f"destinations  : {report['summary']['destination_count']}")
    print("")
    for destination in report["destinations"]:
        print(f"destination   : {destination['destination_id']}")
        print(f"type          : {destination['type']}")
        print(f"model         : {destination.get('model') or '(external command)'}")
        for variant in destination["variants"]:
            usage = variant.get("usage_summary") or {}
            source_tokens = variant.get("source_token_summary") or {}
            final_source_tokens = variant.get("final_source_token_summary") or {}
            exact_final_source_tokens = variant.get("exact_final_source_token_summary") or {}
            python_summary = variant.get("python_baseline_summary") or {}
            python_usage = variant.get("python_baseline_usage_summary") or {}
            python_source_tokens = variant.get("python_baseline_source_token_summary") or {}
            python_final_source_tokens = variant.get("python_baseline_final_source_token_summary") or {}
            python_exact_final_source_tokens = variant.get("python_baseline_exact_final_source_token_summary") or {}
            usage_bits: list[str] = []
            if usage.get("prompt_tokens_total") is not None:
                usage_bits.append(f"workflow_prompt_tok={usage['prompt_tokens_total']}")
            if usage.get("completion_tokens_total") is not None:
                usage_bits.append(f"workflow_completion_tok={usage['completion_tokens_total']}")
            if usage.get("total_tokens_total") is not None:
                usage_bits.append(f"workflow_total_tok={usage['total_tokens_total']}")
            if usage.get("attempts_with_usage"):
                usage_bits.append(
                    f"usage_cov={usage['attempts_with_usage']}/{usage.get('attempts_total', 0)}"
                )
            if source_tokens.get("source_approx_tokens_total") is not None:
                usage_bits.append(f"all_attempt_answer_tok~={source_tokens['source_approx_tokens_total']}")
            if final_source_tokens.get("source_approx_tokens_total") is not None:
                usage_bits.append(f"final_answer_tok~={final_source_tokens['source_approx_tokens_total']}")
            if exact_final_source_tokens.get("source_approx_tokens_total") is not None:
                usage_bits.append(f"exact_final_answer_tok~={exact_final_source_tokens['source_approx_tokens_total']}")
            print(
                f"{variant['doc_name']:>5}  "
                f"approx_tokens={variant['doc_approx_tokens']:<6}  "
                f"generated={variant['summary']['generated_ok']}/{variant['summary']['total_cases']}  "
                f"run={variant['summary']['run_ok']}/{variant['summary']['total_cases']}  "
                f"exact={variant['summary']['exact_stdout_match']}/{variant['summary']['total_cases']}  "
                f"repaired={variant['summary']['resolved_after_repair']}/{variant['summary']['total_cases']}"
            )
            if usage_bits:
                print(f"       usage : {'  '.join(usage_bits)}")
            if python_summary:
                py_bits: list[str] = []
                if python_usage.get("prompt_tokens_total") is not None:
                    py_bits.append(f"workflow_prompt_tok={python_usage['prompt_tokens_total']}")
                if python_usage.get("completion_tokens_total") is not None:
                    py_bits.append(f"workflow_completion_tok={python_usage['completion_tokens_total']}")
                if python_usage.get("total_tokens_total") is not None:
                    py_bits.append(f"workflow_total_tok={python_usage['total_tokens_total']}")
                if python_source_tokens.get("source_approx_tokens_total") is not None:
                    py_bits.append(f"all_attempt_answer_tok~={python_source_tokens['source_approx_tokens_total']}")
                if python_final_source_tokens.get("source_approx_tokens_total") is not None:
                    py_bits.append(f"final_answer_tok~={python_final_source_tokens['source_approx_tokens_total']}")
                if python_exact_final_source_tokens.get("source_approx_tokens_total") is not None:
                    py_bits.append(f"exact_final_answer_tok~={python_exact_final_source_tokens['source_approx_tokens_total']}")
                print(
                    f"       python: generated={python_summary['generated_ok']}/{python_summary['total_cases']}  "
                    f"run={python_summary['run_ok']}/{python_summary['total_cases']}  "
                    f"exact={python_summary['exact_stdout_match']}/{python_summary['total_cases']}  "
                    f"repaired={python_summary['resolved_after_repair']}/{python_summary['total_cases']}"
                )
                if py_bits:
                    print(f"       py use: {'  '.join(py_bits)}")
            for failure in variant.get("failure_patterns", [])[:3]:
                print(
                    f"       fail x{failure['count']}: {failure['fingerprint']} "
                    f"[tasks: {', '.join(failure['task_ids'])}]"
                )
        print("")


def print_progress_start(destination: Destination, doc_name: str, task: Task, repeat_index: int) -> None:
    print(
        f"[progress] {destination.destination_id} {doc_name} {task.task_id} repeat={repeat_index} start",
        file=sys.stderr,
        flush=True,
    )


def print_progress_done(
    destination: Destination,
    doc_name: str,
    task: Task,
    repeat_index: int,
    result: dict[str, Any],
) -> None:
    run = result.get("run", {})
    print(
        f"[progress] {destination.destination_id} {doc_name} {task.task_id} repeat={repeat_index} "
        f"generated={int(bool(result.get('generated_ok', False)))} "
        f"returncode={run.get('returncode', -1)} "
        f"exact={int(bool(run.get('exact_stdout_match', False)))}",
        file=sys.stderr,
        flush=True,
    )


def finalize_case_record(attempts: list[dict[str, Any]]) -> dict[str, Any]:
    final_attempt = attempts[-1]
    case_record = dict(final_attempt)
    case_record["attempts"] = attempts
    case_record["attempt_count"] = len(attempts)
    case_record["resolved_after_repair"] = (
        case_record["run"]["exact_stdout_match"] and len(attempts) > 1
    )
    case_record["failure_fingerprint"] = (
        "" if case_record["run"]["exact_stdout_match"] else derive_failure_fingerprint(case_record)
    )
    return case_record


def apply_repairs(
    *,
    initial_attempt: dict[str, Any],
    destination: Destination,
    task: Task,
    args: argparse.Namespace,
    runner: str,
    repair_prompt_builder: Any,
) -> list[dict[str, Any]]:
    attempts: list[dict[str, Any]] = [initial_attempt]
    attempt = initial_attempt

    if args.repair_attempts > 0 and not attempt["run"]["exact_stdout_match"]:
        for repair_index in range(args.repair_attempts):
            failure_summary = derive_failure_summary(
                generated_ok=attempt.get("generated_ok", False),
                run=attempt["run"],
                generation_error=attempt.get("generation_error"),
            )
            repair_prompt = repair_prompt_builder(
                task=task,
                previous_source=truncate_for_prompt(attempt.get("source_code", ""), args.repair_feedback_limit),
                attempt_number=repair_index + 1,
                failure_summary=failure_summary,
                observed_stdout=truncate_for_prompt(attempt["run"].get("stdout", ""), args.repair_feedback_limit),
                observed_stderr=truncate_for_prompt(attempt["run"].get("stderr", ""), args.repair_feedback_limit),
            )
            attempt = evaluate_attempt(
                prompt=repair_prompt,
                prompt_kind="repair",
                destination=destination,
                task=task,
                args=args,
                runner=runner,
            )
            attempts.append(attempt)
            if attempt["run"]["exact_stdout_match"]:
                break

    return attempts


def execute_case(
    *,
    initial_prompt: str,
    destination: Destination,
    task: Task,
    args: argparse.Namespace,
    runner: str,
    repair_prompt_builder: Any,
) -> dict[str, Any]:
    attempt = evaluate_attempt(
        prompt=initial_prompt,
        prompt_kind="initial",
        destination=destination,
        task=task,
        args=args,
        runner=runner,
    )
    attempts = apply_repairs(
        initial_attempt=attempt,
        destination=destination,
        task=task,
        args=args,
        runner=runner,
        repair_prompt_builder=repair_prompt_builder,
    )
    return finalize_case_record(attempts)


def chunk_list(items: list[Any], size: int) -> list[list[Any]]:
    if size <= 1:
        return [[item] for item in items]
    return [items[idx:idx + size] for idx in range(0, len(items), size)]


def run_aether_cases_for_repeat(
    *,
    destination: Destination,
    doc_name: str,
    doc_text: str,
    tasks: list[Task],
    repeat_index: int,
    args: argparse.Namespace,
    doc_token_reference: dict[str, Any],
    on_case_complete: Any | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    results: list[dict[str, Any]] = []
    batch_runs: list[dict[str, Any]] = []
    batch_size = effective_shared_guide_batch_size(args, destination)

    def run_single_task(task: Task) -> dict[str, Any]:
        if args.progress:
            print_progress_start(destination, doc_name, task, repeat_index)
        prompt = build_prompt(doc_name=doc_name, doc_text=doc_text, task=task)
        case_record: dict[str, Any] = {
            "task_id": task.task_id,
            "task_title": task.title,
            "repeat_index": repeat_index,
            "attempts": [],
            "doc_token_reference": doc_token_reference,
        }
        context_limit = get_destination_context_limit(destination)
        if context_limit is not None and approx_tokens(prompt) >= context_limit:
            case_record.update(
                make_prompt_too_large_record(
                    prompt=prompt,
                    context_limit=context_limit,
                    destination=destination,
                )
            )
            if args.progress:
                print_progress_done(destination, doc_name, task, repeat_index, case_record)
            return case_record
        try:
            aether_case = execute_case(
                initial_prompt=prompt,
                destination=destination,
                task=task,
                args=args,
                runner="aether",
                repair_prompt_builder=lambda **kwargs: build_repair_prompt(
                    doc_name=doc_name,
                    doc_text=doc_text,
                    **kwargs,
                ),
            )
            case_record.update(aether_case)
            case_record["doc_token_reference"] = doc_token_reference
        except Exception as exc:  # pragma: no cover - surfaced in JSON report
            case_record["generated_ok"] = False
            case_record["generation_error"] = str(exc)
            case_record["run"] = {
                "returncode": -1,
                "stdout": "",
                "stderr": str(exc),
                "elapsed_seconds": 0.0,
                "exact_stdout_match": False,
            }
            case_record["attempt_count"] = len(case_record["attempts"])
            case_record["resolved_after_repair"] = False
            case_record["failure_fingerprint"] = derive_failure_fingerprint(case_record)
        finally:
            try:
                run_destination_cleanup(destination, task, doc_name, repeat_index)
            except Exception as cleanup_exc:  # pragma: no cover - surfaced in JSON report
                case_record["cleanup_error"] = str(cleanup_exc)
        if args.progress:
            print_progress_done(destination, doc_name, task, repeat_index, case_record)
        return case_record

    if batch_size <= 1:
        for task in tasks:
            case_record = run_single_task(task)
            results.append(case_record)
            if on_case_complete:
                on_case_complete(case_record)
        return results, batch_runs

    for task_group in chunk_list(tasks, batch_size):
        for task in task_group:
            if args.progress:
                print_progress_start(destination, doc_name, task, repeat_index)

        batch_prompt = build_batch_prompt(doc_name=doc_name, doc_text=doc_text, tasks=task_group)
        context_limit = get_destination_context_limit(destination)
        if context_limit is not None and approx_tokens(batch_prompt) >= context_limit:
            if len(task_group) > 1:
                for task in task_group:
                    case_record = run_single_task(task)
                    results.append(case_record)
                    if on_case_complete:
                        on_case_complete(case_record)
                continue
            case_record = run_single_task(task_group[0])
            results.append(case_record)
            if on_case_complete:
                on_case_complete(case_record)
            continue

        batch_prompt_tokens = approx_tokens(batch_prompt)
        batch_id = f"{doc_name}-r{repeat_index}-{'-'.join(task.task_id for task in task_group)}"
        batch_meta: dict[str, Any] = {
            "batch_id": batch_id,
            "repeat_index": repeat_index,
            "task_ids": [task.task_id for task in task_group],
            "prompt_approx_tokens": batch_prompt_tokens,
            "doc_name": doc_name,
        }

        shared_generation: dict[str, Any] | None = None
        try:
            shared_generation = run_model_with_deadline(batch_prompt, destination)
            shared_usage = normalize_usage(shared_generation.get("usage"))
            split_usage = split_usage_across_tasks(shared_usage, len(task_group))
            split_prompt_tokens = split_int_total(batch_prompt_tokens, len(task_group))
            sources = parse_batch_sources(
                shared_generation.get("raw_text", ""),
                [task.task_id for task in task_group],
            )
            batch_meta["usage"] = shared_usage
            batch_meta["parsed_task_count"] = len(sources)

            for idx, task in enumerate(task_group):
                case_record: dict[str, Any] = {
                    "task_id": task.task_id,
                    "task_title": task.title,
                    "repeat_index": repeat_index,
                    "doc_token_reference": doc_token_reference,
                }
                try:
                    source_code = sources.get(task.task_id, "")
                    generation_error = None
                    if not source_code.strip():
                        generation_error = (
                            f"shared batch did not return source_code for task '{task.task_id}'"
                        )
                    initial_attempt = build_attempt_from_source(
                        task=task,
                        args=args,
                        runner="aether",
                        source_code=source_code,
                        prompt_kind="initial_batch",
                        prompt_approx_tokens=split_prompt_tokens[idx],
                        usage=split_usage[idx],
                        generation_meta={
                            "response_id": shared_generation.get("response_id"),
                            "shared_batch": True,
                            "task_ids": [item.task_id for item in task_group],
                        },
                        generation_error=generation_error,
                        shared_prompt_approx_tokens=batch_prompt_tokens,
                        batch_id=batch_id,
                    )
                    attempts = apply_repairs(
                        initial_attempt=initial_attempt,
                        destination=destination,
                        task=task,
                        args=args,
                        runner="aether",
                        repair_prompt_builder=lambda **kwargs: build_repair_prompt(
                            doc_name=doc_name,
                            doc_text=doc_text,
                            **kwargs,
                        ),
                    )
                    case_record.update(finalize_case_record(attempts))
                    case_record["doc_token_reference"] = doc_token_reference
                except Exception as exc:  # pragma: no cover - surfaced in JSON report
                    case_record["generated_ok"] = False
                    case_record["generation_error"] = str(exc)
                    case_record["run"] = {
                        "returncode": -1,
                        "stdout": "",
                        "stderr": str(exc),
                        "elapsed_seconds": 0.0,
                        "exact_stdout_match": False,
                    }
                    case_record["attempt_count"] = 0
                    case_record["resolved_after_repair"] = False
                    case_record["failure_fingerprint"] = derive_failure_fingerprint(case_record)
                finally:
                    try:
                        run_destination_cleanup(destination, task, doc_name, repeat_index)
                    except Exception as cleanup_exc:  # pragma: no cover - surfaced in JSON report
                        case_record["cleanup_error"] = str(cleanup_exc)
                if args.progress:
                    print_progress_done(destination, doc_name, task, repeat_index, case_record)
                results.append(case_record)
                if on_case_complete:
                    on_case_complete(case_record)
        except Exception as exc:  # pragma: no cover - surfaced in JSON report
            batch_meta["error"] = str(exc)
            for task in task_group:
                case_record = run_single_task(task)
                case_record["batch_fallback_reason"] = str(exc)
                results.append(case_record)
                if on_case_complete:
                    on_case_complete(case_record)

        batch_runs.append(batch_meta)

    return results, batch_runs



def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS, help="task manifest JSON")
    parser.add_argument(
        "--docs",
        default="full,small",
        help="comma-separated doc variants to benchmark (default: full,small; also supports none)",
    )
    parser.add_argument("--task", action="append", default=[], help="restrict to one or more task ids")
    parser.add_argument("--list-tasks", action="store_true", help="list manifest task ids and exit")
    parser.add_argument(
        "--bucket-report",
        action="append",
        default=[],
        type=pathlib.Path,
        help="benchmark report JSON used to classify tasks by failure rate; may be repeated",
    )
    parser.add_argument(
        "--bucket-destination",
        action="append",
        default=[],
        help="restrict task-bucket classification to one or more destination ids in the report(s)",
    )
    parser.add_argument(
        "--bucket-doc",
        action="append",
        default=[],
        help="restrict task-bucket classification to one or more doc variants in the report(s)",
    )
    parser.add_argument(
        "--bucket-metric",
        choices=("generated", "run", "exact"),
        default="exact",
        help="metric used when classifying tasks from prior report(s) (default: exact)",
    )
    parser.add_argument(
        "--bucket-failure-threshold",
        type=float,
        default=0.2,
        help="tasks at or above this failure rate are classified as unstable (default: 0.2)",
    )
    parser.add_argument(
        "--task-bucket",
        choices=("stable", "unstable"),
        default="",
        help="optionally run only tasks classified into this bucket from prior report(s)",
    )
    parser.add_argument(
        "--list-task-buckets",
        action="store_true",
        help="print stable/unstable task classification from prior report(s) and exit",
    )
    parser.add_argument(
        "--destinations-config",
        type=pathlib.Path,
        default=DEFAULT_DESTINATIONS_CONFIG,
        help="destination profile JSON",
    )
    parser.add_argument(
        "--destination",
        action="append",
        default=[],
        help="restrict to one or more destination ids from the config",
    )
    parser.add_argument("--list-destinations", action="store_true", help="list configured destinations and exit")
    parser.add_argument(
        "--temperature",
        type=float,
        default=0.2,
        help="legacy fallback only; use destination configs instead",
    )
    parser.add_argument(
        "--max-output-tokens",
        type=int,
        default=3000,
        help="legacy fallback only; use destination configs instead",
    )
    parser.add_argument(
        "--command-template",
        default="",
        help="legacy fallback only; use destination configs instead",
    )
    parser.add_argument("--provider", default="", help="legacy fallback only; use destination configs instead")
    parser.add_argument("--model", default="", help="legacy fallback only; use destination configs instead")
    parser.add_argument(
        "--request-timeout-seconds",
        type=int,
        default=120,
        help="legacy fallback only; overall model request timeout in seconds",
    )
    parser.add_argument("--repeats", type=int, default=1, help="repeat each task N times per doc variant")
    parser.add_argument(
        "--repair-attempts",
        type=int,
        default=0,
        help="when > 0, retry failed cases by feeding diagnostics back to the model",
    )
    parser.add_argument(
        "--repair-feedback-limit",
        type=int,
        default=1200,
        help="max characters of stdout/stderr/source included in a repair prompt section",
    )
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN, help="path to local aether binary")
    parser.add_argument(
        "--python-baseline",
        action="store_true",
        help="also ask for Python 3 for each case, run it locally, and record token usage for comparison",
    )
    parser.add_argument(
        "--shared-guide-batch-size",
        type=int,
        default=1,
        help="when > 1, batch that many Aether tasks behind one shared guide prompt before splitting results back per case",
    )
    parser.add_argument("--output-json", type=pathlib.Path, default=None, help="write full JSON report to this path")
    parser.add_argument("--text-summary", action="store_true", help="print compact text summary")
    parser.add_argument("--progress", action="store_true", help="print per-task progress to stderr")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    tasks = load_tasks(args.tasks)
    task_bucket_stats: dict[str, dict[str, Any]] = {}
    if args.bucket_report:
        destination_filter = set(args.bucket_destination) if args.bucket_destination else None
        doc_filter = set(args.bucket_doc) if args.bucket_doc else None
        task_bucket_stats = compute_task_bucket_stats(
            report_paths=args.bucket_report,
            metric=args.bucket_metric,
            destination_filter=destination_filter,
            doc_filter=doc_filter,
        )
    elif args.task_bucket or args.list_task_buckets:
        raise SystemExit("--task-bucket and --list-task-buckets require at least one --bucket-report")

    if args.list_task_buckets:
        stable_ids: list[str] = []
        unstable_ids: list[str] = []
        no_data_ids: list[str] = []
        for task in tasks:
            entry = task_bucket_stats.get(task.task_id)
            bucket = classify_task_bucket(entry, args.bucket_failure_threshold) if entry else "no_data"
            line = task.task_id
            if entry:
                line += (
                    f"\tbucket={bucket}\tsamples={entry['samples']}"
                    f"\tsuccess_rate={entry['success_rate']:.2f}\tfailure_rate={entry['failure_rate']:.2f}"
                )
            else:
                line += "\tbucket=no_data\tsamples=0\tsuccess_rate=0.00\tfailure_rate=0.00"
            print(line)
            if bucket == "stable":
                stable_ids.append(task.task_id)
            elif bucket == "unstable":
                unstable_ids.append(task.task_id)
            else:
                no_data_ids.append(task.task_id)
        print("")
        print(f"stable    ({len(stable_ids)}): {', '.join(stable_ids)}")
        print(f"unstable  ({len(unstable_ids)}): {', '.join(unstable_ids)}")
        if no_data_ids:
            print(f"no_data   ({len(no_data_ids)}): {', '.join(no_data_ids)}")
        return 0

    if args.task_bucket:
        wanted_bucket = args.task_bucket
        selected_ids = {
            task.task_id
            for task in tasks
            if task.task_id in task_bucket_stats
            and classify_task_bucket(task_bucket_stats[task.task_id], args.bucket_failure_threshold) == wanted_bucket
        }
        tasks = [task for task in tasks if task.task_id in selected_ids]
    if args.task:
        wanted = set(args.task)
        tasks = [task for task in tasks if task.task_id in wanted]

    if args.list_tasks:
        for task in tasks:
            print(f"{task.task_id}\t{task.title}")
        return 0

    if not tasks:
        raise SystemExit("no tasks selected")

    if args.destinations_config.exists():
        destinations = load_destinations(args.destinations_config)
    else:
        destinations = []

    if not destinations and args.provider:
        destinations = [
            Destination(
                destination_id="legacy-cli",
                kind="openai_responses" if args.provider == "openai" else args.provider,
                model=args.model or None,
                temperature=args.temperature,
                max_output_tokens=args.max_output_tokens,
                command_template=args.command_template or None,
                request_timeout_seconds=args.request_timeout_seconds,
            )
        ]

    if args.destination:
        wanted_destinations = set(args.destination)
        destinations = [item for item in destinations if item.destination_id in wanted_destinations]

    if args.list_destinations:
        for item in destinations:
            model = item.model or "-"
            print(f"{item.destination_id}\t{item.kind}\t{model}")
        return 0

    if not destinations:
        raise SystemExit("no destinations selected")

    doc_variants = resolve_docs([part.strip() for part in args.docs.split(",") if part.strip()])

    report: dict[str, Any] = {
        "tasks_file": str(args.tasks),
        "destinations_config": str(args.destinations_config),
        "created_at_unix": int(time.time()),
        "summary": {
            "total_cases_per_destination": len(tasks) * args.repeats,
            "doc_variants": len(doc_variants),
            "destination_count": len(destinations),
        },
        "doc_token_reference": {
            name: {
                "path": str(path) if path else None,
                "approx_tokens": approx_tokens(read_text(path)) if path else 0,
                "bytes": len(read_text(path).encode("utf-8")) if path else 0,
            }
            for name, path in DOC_VARIANTS.items()
        },
        "destinations": [],
    }

    def persist_report_checkpoint() -> None:
        if not args.output_json:
            return
        report["updated_at_unix"] = int(time.time())
        write_json_atomic(args.output_json, report)

    def refresh_variant_report(variant_report: dict[str, Any]) -> None:
        results = variant_report["results"]
        variant_report["summary"] = summarize(results)
        variant_report["usage_summary"] = summarize_usage(results)
        variant_report["source_token_summary"] = summarize_source_tokens(results)
        variant_report["final_usage_summary"] = summarize_final_usage(results, "all")
        variant_report["run_ok_final_usage_summary"] = summarize_final_usage(results, "run_ok")
        variant_report["exact_final_usage_summary"] = summarize_final_usage(results, "exact")
        variant_report["final_source_token_summary"] = summarize_final_source_tokens(results, "all")
        variant_report["run_ok_final_source_token_summary"] = summarize_final_source_tokens(results, "run_ok")
        variant_report["exact_final_source_token_summary"] = summarize_final_source_tokens(results, "exact")
        variant_report["failure_patterns"] = summarize_failure_patterns(results)

        if "python_baseline_results" in variant_report:
            python_results = variant_report["python_baseline_results"]
            variant_report["python_baseline_summary"] = summarize(python_results)
            variant_report["python_baseline_usage_summary"] = summarize_usage(python_results)
            variant_report["python_baseline_source_token_summary"] = summarize_source_tokens(python_results)
            variant_report["python_baseline_final_usage_summary"] = summarize_final_usage(python_results, "all")
            variant_report["python_baseline_run_ok_final_usage_summary"] = summarize_final_usage(python_results, "run_ok")
            variant_report["python_baseline_exact_final_usage_summary"] = summarize_final_usage(python_results, "exact")
            variant_report["python_baseline_final_source_token_summary"] = summarize_final_source_tokens(python_results, "all")
            variant_report["python_baseline_run_ok_final_source_token_summary"] = summarize_final_source_tokens(python_results, "run_ok")
            variant_report["python_baseline_exact_final_source_token_summary"] = summarize_final_source_tokens(python_results, "exact")
            variant_report["python_failure_patterns"] = summarize_failure_patterns(python_results)

    def append_case_and_checkpoint(variant_report: dict[str, Any], case_record: dict[str, Any]) -> None:
        variant_report["results"].append(case_record)
        refresh_variant_report(variant_report)
        persist_report_checkpoint()

    for destination in destinations:
        destination_report = {
            "destination_id": destination.destination_id,
            "type": destination.kind,
            "model": destination.model,
            "base_url": destination.base_url,
            "variants": [],
        }
        report["destinations"].append(destination_report)
        for doc_name, doc_path in doc_variants:
            doc_text = read_text(doc_path) if doc_path else ""
            variant_report = {
                "doc_name": doc_name,
                "doc_path": str(doc_path) if doc_path else None,
                "doc_bytes": len(doc_text.encode("utf-8")),
                "doc_approx_tokens": approx_tokens(doc_text) if doc_text else 0,
                "shared_guide_batch_size_requested": max(1, int(args.shared_guide_batch_size)),
                "shared_guide_batch_size": effective_shared_guide_batch_size(args, destination),
                "batch_mode_enabled": bool(effective_shared_guide_batch_size(args, destination) > 1),
                "batch_runs": [],
                "results": [],
                "summary": summarize([]),
                "usage_summary": summarize_usage([]),
                "source_token_summary": summarize_source_tokens([]),
                "final_usage_summary": summarize_final_usage([], "all"),
                "run_ok_final_usage_summary": summarize_final_usage([], "run_ok"),
                "exact_final_usage_summary": summarize_final_usage([], "exact"),
                "final_source_token_summary": summarize_final_source_tokens([], "all"),
                "run_ok_final_source_token_summary": summarize_final_source_tokens([], "run_ok"),
                "exact_final_source_token_summary": summarize_final_source_tokens([], "exact"),
                "failure_patterns": summarize_failure_patterns([]),
            }
            if args.python_baseline:
                variant_report["python_baseline_results"] = []
            destination_report["variants"].append(variant_report)
            persist_report_checkpoint()

            for repeat_index in range(args.repeats):
                repeat_results, repeat_batch_runs = run_aether_cases_for_repeat(
                    destination=destination,
                    doc_name=doc_name,
                    doc_text=doc_text,
                    tasks=tasks,
                    repeat_index=repeat_index,
                    args=args,
                    doc_token_reference=report["doc_token_reference"],
                    on_case_complete=lambda case_record, variant_report=variant_report: append_case_and_checkpoint(
                        variant_report,
                        case_record,
                    ),
                )
                variant_report["batch_runs"].extend(repeat_batch_runs)
                refresh_variant_report(variant_report)
                persist_report_checkpoint()

                if args.python_baseline:
                    for task in tasks:
                        python_case = execute_case(
                            initial_prompt=build_python_prompt(task),
                            destination=destination,
                            task=task,
                            args=args,
                            runner="python",
                            repair_prompt_builder=lambda **kwargs: build_python_repair_prompt(**kwargs),
                        )
                        python_case["task_id"] = task.task_id
                        python_case["task_title"] = task.title
                        python_case["repeat_index"] = repeat_index
                        python_case["doc_token_reference"] = report["doc_token_reference"]
                        variant_report["python_baseline_results"].append(python_case)
                        refresh_variant_report(variant_report)
                        persist_report_checkpoint()

    if args.output_json:
        persist_report_checkpoint()

    if args.text_summary or not args.output_json:
        print_text_summary(report)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
