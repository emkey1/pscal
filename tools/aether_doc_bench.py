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
import difflib
import json
import multiprocessing
import os
import pathlib
import queue as queue_module
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import email.utils
import hashlib
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
# The board's simple suite. tasks.json is off the boards (and kept only for
# history); it used to be the default, so a bare run scored the wrong suite.
DEFAULT_TASKS = REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks_v2_pos.json"
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_DESTINATIONS_CONFIG = REPO_ROOT / "Tests" / "aether_doc_bench" / "destinations.template.json"
# The aether checkout the guides (and, for the skew guard, the binary's +sha) come
# from. --aether-root points it elsewhere, e.g. at the versioned ~/aether-<sha>
# checkout a board's binary was built from, so binary and guide share one sha.
DEFAULT_AETHER_ROOT = REPO_ROOT / "components" / "aether"
DOC_FILENAMES: dict[str, str] = {
    "full": "aether_for_llms_and_others.md",
    "medium": "aether_for_llms_medium_contexts.md",
    "small": "aether_for_llms_with_small_contexts.md",
}


def doc_variant_paths(
    aether_root: pathlib.Path | None = None,
    overrides: dict[str, pathlib.Path] | None = None,
) -> dict[str, pathlib.Path | None]:
    """The guide variants: the three tiers under <aether_root>/docs, `none`, and
    every --doc NAME=PATH override (which may add new names or replace a tier)."""
    root = aether_root if aether_root is not None else DEFAULT_AETHER_ROOT
    variants: dict[str, pathlib.Path | None] = {
        name: root / "docs" / filename for name, filename in DOC_FILENAMES.items()
    }
    variants["none"] = None
    if overrides:
        variants.update(overrides)
    return variants


# Kept for callers that read the default map (older tools); new code goes through
# doc_variant_paths() so --aether-root and --doc are honoured.
DOC_VARIANTS: dict[str, pathlib.Path | None] = doc_variant_paths()
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
    reference_solution: str | None = None
    # W1-21. exact_match means returncode == expected_returncode AND stdout ==
    # expected_stdout. stdin is a literal string or {"file": NAME} naming one
    # of `files`. hide_expected_stdout (task- or suite-level; D37a: trap suites
    # only) leaves the Expected stdout block out of the FIRST-attempt prompt,
    # so the task prompt must specify the format completely; repair rounds
    # still show it.
    expected_returncode: int = 0
    stdin: Any = None
    hide_expected_stdout: bool = False


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
    extra_headers: dict | None = None
    preferred_targets: list[str] | None = None
    priority: int = 5
    # Base sampling seed. Repeat r of a case is requested with seed + r, so
    # --repeats N draws N distinct, reproducible samples (D37b: seed 42+r).
    seed: int | None = None


@dataclass
class RequestOptions:
    """Per-request settings the case loop decides (not the destination config):
    the seed for this repeat and, once the context guard has run, the clamped
    output budget. None means "use the destination's own value / send nothing"."""

    seed: int | None = None
    max_tokens: int | None = None


# Destination kinds whose request builders forward a seed. The OpenAI Responses
# API takes no seed, so a seed configured there would be silently dropped --
# exactly the bug this field exists to end -- and is rejected at load instead.
SEEDABLE_KINDS = frozenset(
    {"openai_chat_completions", "openai_completions", "tra_queue", "tra_scheduler", "command"}
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_text(text: str) -> str:
    return sha256_bytes(text.encode("utf-8"))


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def display_path(path: Any) -> str | None:
    """A path as it may be written into a report that can end up in this public
    repo: repo-relative when inside it, `~/...` under the home directory, else
    absolute. Never the account name."""
    if path is None:
        return None
    candidate = pathlib.Path(path)
    try:
        resolved = candidate.resolve()
    except OSError:
        resolved = candidate
    try:
        return str(resolved.relative_to(REPO_ROOT))
    except ValueError:
        pass
    try:
        return "~/" + str(resolved.relative_to(pathlib.Path.home()))
    except ValueError:
        return str(resolved)


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


def guide_version(text: str) -> str | None:
    """The guide's own stamp, e.g. '2026-08-11-3'.

    Each guide carries `*Guide version: YYYY-MM-DD-N*` near the top, bumped per
    revision per day. That stamp is the identity of the document the model was
    shown -- doc_bytes is a proxy that collides (two edits can land on the same
    size) and says nothing about ordering. Recorded per variant so a report
    states which guide produced it instead of leaving it to be inferred.
    """
    if not text:
        return None
    m = re.search(r"(?:Guide|Card) version:\s*([0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]+)", text[:4000])
    return m.group(1) if m else None


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

    # Responses API spells these *_tokens_details; chat completions spells them
    # prompt_tokens_details / completion_tokens_details. Read both so cache and
    # reasoning accounting survives whichever adapter produced the reply.
    input_details = raw_usage.get("input_tokens_details") or raw_usage.get("prompt_tokens_details")
    output_details = raw_usage.get("output_tokens_details") or raw_usage.get("completion_tokens_details")
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
    suite_hide = bool(raw.get("hide_expected_stdout", False)) if isinstance(raw, dict) else False
    tasks: list[Task] = []
    for item in raw["tasks"]:
        if item.get("should_fail"):
            # Negative-tier compiler invariants (a fixed program that must be
            # rejected), checked by tools/aether_oracle_check.py; not tasks.
            continue
        stdin = item.get("stdin")
        if isinstance(stdin, dict) and stdin.get("file") not in (item.get("files") or {}):
            raise SystemExit(f"{path}: task {item.get('id')!r}: stdin file {stdin.get('file')!r} is not in its files")
        tasks.append(
            Task(
                task_id=item["id"],
                title=item["title"],
                prompt=item["prompt"].strip(),
                expected_stdout=item["expected_stdout"],
                timeout_seconds=int(item.get("timeout_seconds", 20)),
                cwd=item.get("cwd"),
                files=item.get("files"),
                reference_solution=item.get("reference_solution"),
                expected_returncode=int(item.get("expected_returncode", 0)),
                stdin=stdin,
                hide_expected_stdout=bool(item.get("hide_expected_stdout", suite_hide)),
            )
        )
    return tasks


def task_stdin_text(task: Task) -> str | None:
    """The text a task feeds the program on stdin, or None (the program then
    reads EOF)."""
    stdin = getattr(task, "stdin", None)
    if stdin is None:
        return None
    if isinstance(stdin, dict):
        return (task.files or {})[stdin["file"]]
    return str(stdin)


def is_exact(task: Task, returncode: int, stdout: str) -> bool:
    return returncode == getattr(task, "expected_returncode", 0) and stdout == task.expected_stdout


def _expand_fleet_refs(value: Any) -> Any:
    """A destination may name a private host as ${VAR}; resolve it from the environment
    or the untracked fleet overlay (tools/fleet_env.py). This repo is public."""
    if isinstance(value, str) and "${" in value:
        tools_dir = str(pathlib.Path(__file__).resolve().parent)
        if tools_dir not in sys.path:
            sys.path.insert(0, tools_dir)
        import fleet_env

        return fleet_env.expand(value)
    return value


# Every key load_destinations reads. Anything else is a typo or a field this
# harness does not implement, and used to be dropped without a word: a top-level
# "seed": 42 on the cs-aug18 destinations never reached a request, and the same
# shape of bug had already hit chat_template_kwargs (acf5cee6). Keys starting
# with "_" are annotations (_note, _tier, ...) and are allowed anywhere.
DESTINATION_KEYS = frozenset({
    "id", "type", "model", "base_url", "api_key", "api_key_env", "temperature",
    "max_output_tokens", "command_template", "after_each_command",
    "after_each_timeout_seconds", "cooldown_seconds", "prompt_context_limit",
    "request_timeout_seconds", "request_max_retries", "retry_backoff_seconds",
    "extra_body", "extra_headers", "preferred_targets", "priority", "seed",
})
# Read from the raw config by tools/aether_idea_miner.py, not by this harness.
MINER_DESTINATION_KEYS = frozenset({"guide", "system"})
DESTINATION_FILE_KEYS = frozenset({"destinations"})


def validate_destination_config(raw: Any, path: pathlib.Path) -> None:
    """Fail loudly on a destination key nothing reads (see DESTINATION_KEYS)."""
    if not isinstance(raw, dict):
        raise SystemExit(f"{path}: a destinations file must be a JSON object")
    unknown_top = sorted(k for k in raw if k not in DESTINATION_FILE_KEYS and not str(k).startswith("_"))
    if unknown_top:
        raise SystemExit(
            f"{path}: unknown top-level key(s) {', '.join(unknown_top)}; "
            "per-destination settings belong inside each destinations[] entry"
        )
    allowed = DESTINATION_KEYS | MINER_DESTINATION_KEYS
    for index, item in enumerate(raw.get("destinations", [])):
        if not isinstance(item, dict):
            raise SystemExit(f"{path}: destinations[{index}] is not an object")
        label = item.get("id", f"destinations[{index}]")
        unknown = sorted(k for k in item if k not in allowed and not str(k).startswith("_"))
        if unknown:
            raise SystemExit(
                f"{path}: destination {label!r} has unknown key(s) {', '.join(unknown)} "
                f"(known: {', '.join(sorted(allowed))}; prefix a key with '_' for a note)"
            )
        for required in ("id", "type"):
            if required not in item:
                raise SystemExit(f"{path}: destination {label!r} is missing {required!r}")
        seed = item.get("seed")
        if seed is not None:
            if isinstance(seed, bool) or not isinstance(seed, int):
                raise SystemExit(f"{path}: destination {label!r}: seed must be an integer, got {seed!r}")
            if item["type"] not in SEEDABLE_KINDS:
                raise SystemExit(
                    f"{path}: destination {label!r}: type {item['type']!r} cannot be seeded "
                    "(its API takes no seed); remove the seed key"
                )
            extra_body = item.get("extra_body") or {}
            if isinstance(extra_body, dict) and "seed" in extra_body:
                raise SystemExit(
                    f"{path}: destination {label!r} sets both seed and extra_body.seed; keep the "
                    "top-level seed (the harness sends seed + repeat_index)"
                )


def load_destinations(path: pathlib.Path) -> list[Destination]:
    raw = json.loads(read_text(path))
    validate_destination_config(raw, path)
    items = raw.get("destinations", [])
    destinations: list[Destination] = []
    for item in items:
        destinations.append(
            Destination(
                destination_id=item["id"],
                kind=item["type"],
                model=item.get("model"),
                base_url=_expand_fleet_refs(item.get("base_url")),
                api_key=item.get("api_key"),
                api_key_env=item.get("api_key_env"),
                temperature=float(item.get("temperature", 0.2)),
                max_output_tokens=int(item.get("max_output_tokens", 3000)),
                command_template=_expand_fleet_refs(item.get("command_template")),
                after_each_command=_expand_fleet_refs(item.get("after_each_command")),
                after_each_timeout_seconds=int(item.get("after_each_timeout_seconds", 60)),
                cooldown_seconds=float(item.get("cooldown_seconds", 0.0)),
                prompt_context_limit=(
                    int(item["prompt_context_limit"]) if item.get("prompt_context_limit") is not None else None
                ),
                request_timeout_seconds=int(item.get("request_timeout_seconds", 120)),
                request_max_retries=int(item.get("request_max_retries", 0)),
                retry_backoff_seconds=float(item.get("retry_backoff_seconds", 2.0)),
                extra_body=item.get("extra_body"),
                extra_headers=item.get("extra_headers"),
                preferred_targets=item.get("preferred_targets"),
                priority=int(item.get("priority", 5)),
                seed=item.get("seed"),
            )
        )
    return destinations


def is_self_hosted(destination: Destination) -> bool:
    """A destination served on the fleet rather than by a cloud vendor: a local
    command, the T'Ra queue, or a plain-http endpoint. Cloud APIs are https."""
    if destination.kind in ("command", "tra_queue", "tra_scheduler"):
        return True
    return bool(destination.base_url) and str(destination.base_url).startswith("http://")


def request_seed(destination: Destination, repeat_index: int, seed_base: int | None = None) -> int | None:
    """The seed for repeat `repeat_index` (D37b: seed 42 + r).

    A destination's own `seed` is its base. Otherwise the run-wide --seed-base
    applies to self-hosted destinations only: cloud vendors either ignore a seed
    or reject the field, so a cloud destination must opt in explicitly. None when
    no base applies or the destination kind cannot carry a seed."""
    if destination.kind not in SEEDABLE_KINDS:
        return None
    if destination.seed is not None:
        base: int | None = destination.seed
    elif seed_base is not None and is_self_hosted(destination):
        base = seed_base
    else:
        base = None
    if base is None:
        return None
    return int(base) + int(repeat_index)


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


def parse_doc_overrides(items: list[str]) -> dict[str, pathlib.Path]:
    """Parse repeatable --doc NAME=PATH. NAME may be a tier (full/medium/small) to
    replace its default path, or a new name for an A/B variant."""
    overrides: dict[str, pathlib.Path] = {}
    for item in items or []:
        name, sep, raw_path = str(item).partition("=")
        name, raw_path = name.strip(), raw_path.strip()
        if not sep or not name or not raw_path:
            raise SystemExit(f"--doc expects NAME=PATH, got {item!r}")
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
            raise SystemExit(f"--doc {name!r}: a variant name may use letters, digits and _.+- only")
        if name == "none":
            raise SystemExit("--doc none=PATH is not allowed: 'none' is the no-guide variant")
        path = pathlib.Path(raw_path).expanduser()
        if not path.is_file():
            raise SystemExit(f"--doc {name}: guide file not found: {raw_path}")
        if name in overrides:
            raise SystemExit(f"--doc {name} given twice")
        overrides[name] = path.resolve()
    return overrides


def resolve_docs(
    names: list[str],
    variants: dict[str, pathlib.Path | None] | None = None,
) -> list[tuple[str, pathlib.Path | None]]:
    variants = DOC_VARIANTS if variants is None else variants
    resolved: list[tuple[str, pathlib.Path | None]] = []
    seen: set[str] = set()
    for name in names:
        if name not in variants:
            raise SystemExit(f"unknown doc variant '{name}', expected one of: {', '.join(variants)}")
        if name in seen:
            raise SystemExit(f"doc variant '{name}' selected twice")
        seen.add(name)
        path = variants[name]
        if path is not None and not pathlib.Path(path).is_file():
            raise SystemExit(
                f"doc variant '{name}': guide file not found: {path} "
                "(pass --aether-root DIR or --doc NAME=PATH)"
            )
        resolved.append((name, path))
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
    # Many models ignore OUT-001 and wrap the program in a Markdown code fence,
    # often with prose before and/or after it (a leading ```aether and a trailing
    # ``` followed by an explanation). Extract the contents of the FIRST fenced
    # block and discard everything outside it. No-op for models that already
    # return raw source (no fence -> the regex never matches).
    fence = re.search(r"```[ \t]*[A-Za-z0-9_.+-]*[ \t]*\r?\n(.*?)\r?\n[ \t]*```", text, re.S)
    if fence:
        text = fence.group(1).strip()
    else:
        lines = text.splitlines()
        if lines and lines[0].lstrip().startswith("```"):
            lines = lines[1:]
        while lines and lines[-1].strip() == "```":
            lines.pop()
        text = "\n".join(lines).strip()
    # Drop a stray leading bare language tag (e.g. a line "aether") if present.
    lines = text.splitlines()
    if lines and lines[0].strip().lower() in ("aether", "rust", "python", "c", "go", "text"):
        text = "\n".join(lines[1:]).strip()
    return text


def build_prompt(doc_name: str, doc_text: str, task: Task) -> str:
    """The first-attempt prompt. Byte-identical to every earlier board for a
    task with no W1-21 flags; a hidden or non-zero-exit task is adapted."""
    return adapt_prompt_for_task(_build_prompt_plain(doc_name, doc_text, task), task, "Aether source:", hide=True)


def build_python_prompt(task: Task) -> str:
    return adapt_prompt_for_task(_build_python_prompt_plain(task), task, "Python source:", hide=True)


def build_rust_prompt(task: Task) -> str:
    return adapt_prompt_for_task(_build_rust_prompt_plain(task), task, "Rust source:", hide=True)


def adapt_prompt_for_task(prompt: str, task: Task, final_label: str, hide: bool) -> str:
    """Apply a task's W1-21 flags to a rendered prompt.

    hide=True (first attempts only, D37a) drops the Expected stdout block when
    the task asks for it, and the requirement points at the task's own format
    instead. A non-zero expected_returncode adds an Expected exit status block
    and requirement (shown in repair rounds too). A task with neither flag is
    returned unchanged, so every existing suite keeps today's prompts."""
    hidden = hide and getattr(task, "hide_expected_stdout", False)
    status = int(getattr(task, "expected_returncode", 0) or 0)
    if not hidden and not status:
        return prompt
    start = prompt.rfind("Expected stdout:\n")
    label_at = prompt.rfind(final_label)
    if start == -1 or label_at == -1 or label_at < start:
        return prompt
    line_start = prompt.rfind("\n", 0, label_at) + 1
    block_start = prompt.rfind("\n", 0, start) + 1
    indent = prompt[line_start:label_at]
    expected_block = prompt[block_start:line_start]
    replacement = "" if hidden else expected_block
    if status:
        replacement += f"{indent}Expected exit status:\n{indent}{status}\n\n"
    prompt = prompt[:block_start] + replacement + prompt[line_start:]
    if hidden:
        prompt = prompt.replace(
            "- The program must print exactly the expected output.",
            "- The program must print exactly the output the task specifies, in exactly that format.",
        )
    if status:
        prompt = prompt.replace(
            "- The program must print exactly the expected output.",
            f"- The program must print exactly the expected output and exit with status {status}.",
        ).replace(
            "- The program must print exactly the output the task specifies, in exactly that format.",
            "- The program must print exactly the output the task specifies, in exactly that format, "
            f"and exit with status {status}.",
        )
    return prompt


def _build_prompt_plain(doc_name: str, doc_text: str, task: Task) -> str:
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


def _build_python_prompt_plain(task: Task) -> str:
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


def _build_rust_prompt_plain(task: Task) -> str:
    return textwrap.dedent(
        f"""\
        You are writing Rust code.

        Write exactly one complete Rust program (a single `fn main()`, no external crates)
        that solves the task below.

        Requirements:
        - Return only raw Rust source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - After the full program, output a final line containing exactly `{OUTPUT_END_MARKER}`.
        - Keep the program self-contained unless the task explicitly provides files.
        - The program must compile with `rustc` (stable, no external crates) and run.
        - The program must print exactly the expected output.

        Task ID: {task.task_id}
        Task Title: {task.title}
        Task:
        {task.prompt}

        Expected stdout:
        {task.expected_stdout}

        Rust source:
        """
    )


def build_batch_prompt(doc_name: str, doc_text: str, tasks: list[Task]) -> str:
    task_sections: list[str] = []
    for task in tasks:
        # Indentation kept exactly as it always was: when dedent finds no
        # common prefix (multi-line content), it shows up in the prompt.
        section = textwrap.dedent(
                f"""\
                Task ID: {task.task_id}
                Task Title: {task.title}
                Task:
                {task.prompt}

                Expected stdout:
                {task.expected_stdout}
                """
            ).strip()
        if getattr(task, "hide_expected_stdout", False):
            section = section[: section.rfind("Expected stdout:")].rstrip()
        if getattr(task, "expected_returncode", 0):
            section += f"\n\nExpected exit status:\n{task.expected_returncode}"
        task_sections.append(section)

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


def build_repair_prompt(**kwargs: Any) -> str:
    """Repair rounds always show the expected stdout (D37a option b); a
    non-zero expected exit status is added."""
    return adapt_prompt_for_task(_build_repair_prompt_plain(**kwargs), kwargs["task"], "Corrected Aether source:", hide=False)


def _build_repair_prompt_plain(
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


def build_python_repair_prompt(**kwargs: Any) -> str:
    """Repair rounds always show the expected stdout (D37a option b); a
    non-zero expected exit status is added."""
    return adapt_prompt_for_task(_build_python_repair_prompt_plain(**kwargs), kwargs["task"], "Corrected Python source:", hide=False)


def _build_python_repair_prompt_plain(
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


def build_rust_repair_prompt(**kwargs: Any) -> str:
    """Repair rounds always show the expected stdout (D37a option b); a
    non-zero expected exit status is added."""
    return adapt_prompt_for_task(_build_rust_repair_prompt_plain(**kwargs), kwargs["task"], "Corrected Rust source:", hide=False)


def _build_rust_repair_prompt_plain(
    task: Task,
    previous_source: str,
    attempt_number: int,
    failure_summary: str,
    observed_stdout: str,
    observed_stderr: str,
) -> str:
    return textwrap.dedent(
        f"""\
        You are repairing a failed Rust program.

        Return one full corrected Rust program.

        Requirements:
        - Return only raw Rust source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - After the full program, output a final line containing exactly `{OUTPUT_END_MARKER}`.
        - Keep the program self-contained unless the task explicitly provides files.
        - The program must compile with `rustc` (stable, no external crates) and run.
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

        Corrected Rust source:
        """
    )


def resolve_api_key(destination: Destination) -> str | None:
    if destination.api_key is not None:
        return destination.api_key
    if destination.api_key_env:
        return os.environ.get(destination.api_key_env)
    return os.environ.get("OPENAI_API_KEY")


def strip_reasoning_block(text: str) -> str:
    """Drop a reasoning/analysis block from reasoning models.

    Two formats are handled:

    * ``<think>...</think>`` (e.g. Qwen3.5). With the chat template's generation
      prompt the opening ``<think>`` may already be consumed, so the reply can
      start with the closing tag alone. Remove everything up to and including the
      first ``</think>``.
    * OpenAI *harmony* channels (e.g. gpt-oss served via Ollama), which Ollama
      flattens into ``content``: ``<analysis text><|end|><|start|>assistant
      <|channel|>final<|message|><final answer>``. Keep only the text after the
      last ``final`` channel marker -- that is the answer -- dropping the analysis
      (reasoning) channel and any trailing harmony control tokens. The channel
      name may be followed by an explicit ``<|message|>`` token (short replies)
      or just a newline before the answer (longer replies); both are handled.
      (Pair this with ``"extra_body": {"stop": null}`` on the destination so the
      bench end-marker cannot truncate the reply inside the analysis channel
      before ``final``.)

    No-op for models that emit neither.
    """
    final_tag = "<|channel|>final"
    if final_tag in text:
        text = text.rsplit(final_tag, 1)[1].lstrip()
        if text.startswith("<|message|>"):
            text = text[len("<|message|>"):].lstrip()
        for control in ("<|return|>", "<|end|>", "<|start|>", "<|call|>", "<|channel|>"):
            cut = text.find(control)
            if cut != -1:
                text = text[:cut]
        return text.strip()
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
    extra_headers: dict | None = None,
) -> dict[str, Any]:
    headers = {
        "Content-Type": "application/json",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    if extra_headers:
        headers.update({str(k): str(v) for k, v in extra_headers.items()})
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


class ContextOverflowError(RuntimeError):
    """The prompt plus the minimum output budget does not fit the context."""


# The context guard: measured prompt tokens + the output budget + this margin
# (chat-template tokens, tokenizer disagreement) must fit the window.
CONTEXT_MARGIN_TOKENS = 512
MIN_OUTPUT_TOKENS = 1024
CHARS_PER_TOKEN_FALLBACK = 3.4  # measured 3.43-3.47 chars/token on the guides
_TOKEN_COUNT_CACHE: dict[tuple[str, str], tuple[int, str]] = {}
_TOKENIZE_METHOD_CACHE: dict[str, str | None] = {}
_CONTEXT_SOURCE_CACHE: dict[str, tuple[int | None, str | None]] = {}
_O200K: Any = None
_PROMPT_GAP_WARNED: set[str] = set()


def _o200k_encoding() -> Any:
    """tiktoken's o200k_base when importable (and its BPE file is cached), else False."""
    global _O200K
    if _O200K is None:
        try:
            import tiktoken  # type: ignore

            _O200K = tiktoken.get_encoding("o200k_base")
        except Exception:
            _O200K = False
    return _O200K


def tokens_o200k(text: str) -> int | None:
    encoding = _o200k_encoding()
    if not encoding:
        return None
    try:
        return len(encoding.encode(text, disallowed_special=()))
    except Exception:
        return None


def _server_root(base_url: str) -> str:
    root = base_url.rstrip("/")
    return root[:-3] if root.endswith("/v1") else root


def _provider_tokenize(text: str, destination: Destination) -> tuple[int, str] | None:
    """Count with the serving stack's own tokenizer: llama.cpp `POST /tokenize
    {"content"}` or vLLM `POST /tokenize {"model", "prompt"}`. Only tried on
    self-hosted http endpoints (never a cloud API); the working shape is cached."""
    if destination.kind not in ("openai_chat_completions", "openai_completions"):
        return None
    if not destination.base_url or not str(destination.base_url).startswith("http://"):
        return None
    key = destination.destination_id
    method = _TOKENIZE_METHOD_CACHE.get(key, "probe")
    if method is None:
        return None
    root = _server_root(destination.base_url)
    shapes = [method] if method != "probe" else ["llama_cpp", "vllm"]
    for shape in shapes:
        body = {"content": text} if shape == "llama_cpp" else {"model": destination.model, "prompt": text}
        try:
            payload = http_json_request(f"{root}/tokenize", body, resolve_api_key(destination), timeout_seconds=30)
        except Exception:
            continue
        count = None
        if isinstance(payload, dict):
            if isinstance(payload.get("count"), int):
                count = payload["count"]
            elif isinstance(payload.get("tokens"), list):
                count = len(payload["tokens"])
        if count is not None:
            _TOKENIZE_METHOD_CACHE[key] = shape
            return count, f"provider_tokenize:{shape}"
    _TOKENIZE_METHOD_CACHE[key] = None
    return None


def count_prompt_tokens(text: str, destination: Destination | None = None) -> tuple[int, str]:
    """(prompt tokens, estimator): the provider's tokenize endpoint where it
    exists, else tiktoken o200k, else chars/3.4. chars/4 (approx_tokens, kept
    for the historical *_approx_tokens fields) undercounts the guides by 15-17%."""
    cache_key = (destination.destination_id if destination else "", sha256_text(text))
    if cache_key in _TOKEN_COUNT_CACHE:
        return _TOKEN_COUNT_CACHE[cache_key]
    result: tuple[int, str] | None = None
    if destination is not None:
        result = _provider_tokenize(text, destination)
    if result is None:
        count = tokens_o200k(text)
        if count is not None:
            result = (count, "tiktoken_o200k")
    if result is None:
        # ceil(chars / 3.4) in integers (float floor division undercounts 340/3.4).
        result = (max(1, -(-len(text) * 10 // 34)), "chars_div_3.4")
    _TOKEN_COUNT_CACHE[cache_key] = result
    return result


def _context_from_record(item: dict[str, Any]) -> int | None:
    for key in ("loaded_context_length", "max_model_len", "n_ctx", "context_length",
                "context_window", "ctx", "max_context_length"):
        value = item.get(key)
        if isinstance(value, (int, float)) and value > 0:
            return int(value)
    for nested in ("meta", "default_generation_settings", "settings", "config"):
        inner = item.get(nested)
        if isinstance(inner, dict):
            value = _context_from_record(inner)
            if value:
                return value
    return None


def _autodetect_context(destination: Destination) -> tuple[int | None, str | None]:
    if destination.kind in ("tra_queue", "tra_scheduler") and destination.base_url:
        # T'Ra lists its targets; take the context of the preferred one.
        eb = destination.extra_body or {}
        wanted = list(eb.get("preferred_targets") or destination.preferred_targets or [])
        for key in ("target_name", "target"):
            if eb.get(key):
                wanted.insert(0, eb[key])
        try:
            payload = http_json_get(destination.base_url.rstrip("/") + "/api/targets", None)
        except Exception:
            return None, None
        items = payload if isinstance(payload, list) else (payload.get("targets") or payload.get("data") or [])
        for item in items if isinstance(items, list) else []:
            if not isinstance(item, dict):
                continue
            name = item.get("name") or item.get("id") or item.get("target")
            if wanted and name not in wanted:
                continue
            value = _context_from_record(item)
            if value:
                return value, "tra_targets"
        return None, None
    if destination.kind not in ("openai_chat_completions", "openai_completions"):
        return None, None
    if not destination.base_url or not str(destination.base_url).startswith("http://"):
        return None, None
    base = destination.base_url.rstrip("/")
    root = _server_root(base)
    api_key = resolve_api_key(destination)
    probes = (
        (f"{root}/api/v0/models", "lmstudio_api"),  # LM Studio: loaded_context_length
        (f"{base}/models", "models_api"),  # vLLM: max_model_len; llama.cpp: meta
        (f"{root}/props", "llama_cpp_props"),  # llama.cpp: default_generation_settings.n_ctx
    )
    for url, source in probes:
        try:
            payload = http_json_get(url, api_key)
        except Exception:
            continue
        if not isinstance(payload, dict):
            continue
        items = payload.get("data")
        if isinstance(items, list):
            for item in items:
                if isinstance(item, dict) and item.get("id") in (destination.model, None):
                    value = _context_from_record(item)
                    if value:
                        return value, source
        value = _context_from_record(payload) if source == "llama_cpp_props" else None
        if value:
            return value, source
    return None, None


def resolve_context_limit(destination: Destination) -> tuple[int | None, str | None]:
    """(context window, where it came from). The destination's
    prompt_context_limit wins; otherwise ask the serving stack once."""
    if destination.prompt_context_limit is not None:
        return int(destination.prompt_context_limit), "config"
    key = destination.destination_id
    if key not in _CONTEXT_SOURCE_CACHE:
        _CONTEXT_SOURCE_CACHE[key] = _autodetect_context(destination)
    return _CONTEXT_SOURCE_CACHE[key]


def get_destination_context_limit(destination: Destination) -> int | None:
    return resolve_context_limit(destination)[0]


def context_limit_required(destination: Destination) -> bool:
    """A self-hosted model endpoint runs the guide near its window (medium plus
    24K of output overflows a 32K Qwen lane), so its context must be known.
    Cloud APIs and local command stand-ins are exempt."""
    if destination.kind in ("tra_queue", "tra_scheduler"):
        return True
    return destination.kind in ("openai_chat_completions", "openai_completions") and is_self_hosted(destination)


def fit_request(
    prompt: str,
    destination: Destination,
    options: RequestOptions | None,
    margin: int = CONTEXT_MARGIN_TOKENS,
    min_output: int = MIN_OUTPUT_TOKENS,
) -> tuple[RequestOptions, dict[str, Any]]:
    """Apply the context guard to one request: measured prompt tokens + output
    budget + margin must fit. The budget is clamped to what is left (and the
    clamp recorded); below min_output the request is not sent at all."""
    prompt_tokens, method = count_prompt_tokens(prompt, destination)
    limit, source = resolve_context_limit(destination)
    requested = _max_tokens_for(destination, options)
    fit: dict[str, Any] = {
        "context_limit": limit,
        "context_source": source,
        "prompt_tokens_measured": prompt_tokens,
        "prompt_tokens_method": method,
        "margin": margin,
        "max_output_tokens": requested,
        "max_tokens_sent": requested,
        "clamped": False,
    }
    if limit is not None:
        available = int(limit) - prompt_tokens - margin
        if available < min_output:
            fit["max_tokens_sent"] = None
            raise ContextOverflowError(
                f"context_overflow: prompt {prompt_tokens} tokens ({method}) + margin {margin} leaves "
                f"{available} of the {limit}-token context for output, below the {min_output} minimum"
            )
        if requested > available:
            fit["max_tokens_sent"] = available
            fit["clamped"] = True
    new_options = RequestOptions(
        seed=None if options is None else options.seed,
        max_tokens=fit["max_tokens_sent"] if fit["clamped"] else (None if options is None else options.max_tokens),
    )
    return new_options, fit


def record_prompt_token_gap(attempt: dict[str, Any], destination: Destination) -> None:
    """Compare the measured prompt with the provider's usage.prompt_tokens and
    warn (once per destination) when they differ by more than 5%."""
    usage = attempt.get("usage") or {}
    reported = usage.get("prompt_tokens") if isinstance(usage, dict) else None
    measured = (attempt.get("context_fit") or {}).get("prompt_tokens_measured")
    if not isinstance(reported, int) or not isinstance(measured, int) or reported <= 0:
        return
    gap = (measured - reported) / reported
    attempt["prompt_token_gap"] = round(gap, 4)
    if abs(gap) > 0.05 and destination.destination_id not in _PROMPT_GAP_WARNED:
        _PROMPT_GAP_WARNED.add(destination.destination_id)
        print(
            f"[context] WARNING {destination.destination_id}: measured prompt {measured} tokens "
            f"({attempt['context_fit'].get('prompt_tokens_method')}) vs provider usage {reported} "
            f"({gap:+.1%}); the context guard may be off -- set prompt_context_limit conservatively",
            file=sys.stderr,
        )


def _max_tokens_for(destination: Destination, options: RequestOptions | None) -> int:
    if options is not None and options.max_tokens is not None:
        return int(options.max_tokens)
    return int(destination.max_output_tokens)


def _apply_output_clamp(body: dict[str, Any], options: RequestOptions | None) -> None:
    """Hold an explicit extra_body max_completion_tokens to the clamped budget too."""
    if options is None or options.max_tokens is None:
        return
    if "max_completion_tokens" in body and isinstance(body["max_completion_tokens"], int):
        body["max_completion_tokens"] = min(body["max_completion_tokens"], int(options.max_tokens))


def invoke_openai_responses(
    prompt: str, destination: Destination, options: RequestOptions | None = None
) -> dict[str, Any]:
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
        "max_output_tokens": _max_tokens_for(destination, options),
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature
    if destination.extra_body:
        body.update(destination.extra_body)
    if options is not None and options.max_tokens is not None:
        body["max_output_tokens"] = min(int(body.get("max_output_tokens") or options.max_tokens), int(options.max_tokens))

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
        # The top-level `output_text` convenience field isn't populated by every
        # model generation (observed empty for gpt-5.4/5.5/5.6-class models even on
        # a `status: completed` reply with real content) -- fall back to extracting
        # text directly from the `output` array's message item(s).
        parts: list[str] = []
        for item in payload.get("output", []):
            if not isinstance(item, dict) or item.get("type") != "message":
                continue
            for chunk in item.get("content", []):
                if isinstance(chunk, dict) and chunk.get("type") in ("output_text", "text"):
                    parts.append(str(chunk.get("text", "")))
        output_text = "".join(parts)
    if not output_text:
        raise RuntimeError("Responses API reply did not contain output_text")

    return {
        "raw_text": output_text,
        "response_id": payload.get("id"),
        "usage": payload.get("usage"),
        "finish_reason": _responses_finish_reason(payload),
    }


def _responses_finish_reason(payload: dict[str, Any]) -> str | None:
    details = payload.get("incomplete_details")
    if isinstance(details, dict) and details.get("reason"):
        return str(details["reason"])
    status = payload.get("status")
    return "stop" if status == "completed" else status


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


def invoke_openai_chat_completions(
    prompt: str, destination: Destination, options: RequestOptions | None = None
) -> dict[str, Any]:
    if not destination.model:
        raise RuntimeError("destination model is required for openai_chat_completions")
    base_url = (destination.base_url or "https://api.openai.com/v1").rstrip("/")
    api_key = resolve_api_key(destination)

    body = {
        "model": destination.model,
        "messages": [
            {"role": "user", "content": prompt},
        ],
        "max_tokens": _max_tokens_for(destination, options),
        "stop": [OUTPUT_END_MARKER],
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature
    if destination.extra_body:
        body.update(destination.extra_body)
    if options is not None and options.seed is not None:
        body["seed"] = int(options.seed)
    _apply_output_clamp(body, options)
    # OpenAI reasoning models (o-series, gpt-5) reject max_tokens; honor
    # max_completion_tokens from extra_body and drop the incompatible field.
    if "max_completion_tokens" in body:
        body.pop("max_tokens", None)
    if body.get("stop") is None:
        body.pop("stop", None)

    payload = http_json_request(
        f"{base_url}/chat/completions",
        body,
        api_key,
        timeout_seconds=destination.request_timeout_seconds,
        max_retries=destination.request_max_retries,
        retry_backoff_seconds=destination.retry_backoff_seconds,
        extra_headers=destination.extra_headers,
    )
    choices = payload.get("choices") or []
    if not choices:
        raise RuntimeError("chat completions reply did not contain choices")
    finish_reason = choices[0].get("finish_reason")
    message = choices[0].get("message") or {}
    output_text = flatten_chat_content(message.get("content"))
    if not output_text and finish_reason != "length":
        # Some reasoning-model chat templates (observed with GLM-4.7-Flash via
        # llama-server) intermittently leave `content` empty and put the entire
        # reply -- reasoning AND the actual answer -- in `reasoning_content`
        # instead. sanitize_code() already knows how to pull a fenced code block
        # or a post-</think> answer out of a raw reasoning blob, so fall back to
        # it rather than discarding a real answer as a failure.
        #
        # Spelling is not portable: llama-server/GLM use `reasoning_content`,
        # vLLM's deepseek_v4 reasoning parser uses plain `reasoning`. Check both,
        # or the fallback silently never fires against one of them.
        #
        # Guarded on finish_reason: a reply truncated at max_tokens has no answer
        # in it at all, so salvaging the partial reasoning would turn an honest
        # infrastructure error into a bogus compile failure charged to the model.
        output_text = flatten_chat_content(
            message.get("reasoning_content")
        ) or flatten_chat_content(message.get("reasoning"))
    if not output_text:
        if finish_reason == "length":
            raise RuntimeError(
                "chat completions reply hit max_tokens "
                f"({destination.max_output_tokens}) before emitting any content -- "
                "the model was still reasoning when the budget ran out; "
                "raise max_output_tokens for this destination"
            )
        raise RuntimeError("chat completions reply did not contain message content")

    return {
        "raw_text": output_text,
        "response_id": payload.get("id"),
        "usage": payload.get("usage"),
        "finish_reason": finish_reason,
    }


def invoke_openai_chat_completions_messages(
    messages: list[dict[str, str]], destination: Destination, options: RequestOptions | None = None
) -> dict[str, Any]:
    """Like invoke_openai_chat_completions but takes a pre-built messages list
    instead of a single flat prompt -- used by aether_doc_bench_session.py's
    growing-transcript session mode. Same request/response handling otherwise."""
    if not destination.model:
        raise RuntimeError("destination model is required for openai_chat_completions")
    base_url = (destination.base_url or "https://api.openai.com/v1").rstrip("/")
    api_key = resolve_api_key(destination)

    body = {
        "model": destination.model,
        "messages": messages,
        "max_tokens": _max_tokens_for(destination, options),
        "stop": [OUTPUT_END_MARKER],
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature
    if destination.extra_body:
        body.update(destination.extra_body)
    if options is not None and options.seed is not None:
        body["seed"] = int(options.seed)
    _apply_output_clamp(body, options)
    if "max_completion_tokens" in body:
        body.pop("max_tokens", None)
    if body.get("stop") is None:
        body.pop("stop", None)

    payload = http_json_request(
        f"{base_url}/chat/completions",
        body,
        api_key,
        timeout_seconds=destination.request_timeout_seconds,
        max_retries=destination.request_max_retries,
        retry_backoff_seconds=destination.retry_backoff_seconds,
        extra_headers=destination.extra_headers,
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
        "finish_reason": choices[0].get("finish_reason"),
    }


def invoke_openai_responses_session(
    prompt: str, destination: Destination, previous_response_id: str | None = None
) -> dict[str, Any]:
    """Like invoke_openai_responses but threads previous_response_id for true
    server-side conversation state -- used by aether_doc_bench_session.py.
    When previous_response_id is set, `prompt` should be ONLY the new turn's
    text (no guide re-inclusion); OpenAI resolves prior turns server-side."""
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
    if previous_response_id:
        body["previous_response_id"] = previous_response_id
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature
    if destination.extra_body:
        body.update(destination.extra_body)

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
        parts: list[str] = []
        for item in payload.get("output", []):
            if not isinstance(item, dict) or item.get("type") != "message":
                continue
            for chunk in item.get("content", []):
                if isinstance(chunk, dict) and chunk.get("type") in ("output_text", "text"):
                    parts.append(str(chunk.get("text", "")))
        output_text = "".join(parts)
    if not output_text:
        raise RuntimeError("Responses API reply did not contain output_text")

    return {
        "raw_text": output_text,
        "response_id": payload.get("id"),
        "usage": payload.get("usage"),
        "finish_reason": _responses_finish_reason(payload),
    }


def invoke_tra_queue(
    prompt: str, destination: Destination, options: RequestOptions | None = None
) -> dict[str, Any]:
    """Canonical T'Ra AI queue adapter -- shared by the doc-bench AND the idea-miner.

    Submit a generation to the queue (e.g. http://m4t:8793), explain-validate, submit
    idempotently (a re-submit of the same prompt reuses the job rather than orphaning a
    duplicate), long-poll, and cancel on give-up so a job never strands a GPU. Resilient
    to a busy shared queue: a hard routing failure bails early, but transient submit/poll
    errors retry. Config comes from the destination fields (model, preferred_targets,
    priority, temperature, timeouts) with ``extra_body`` overrides: ``disable_thinking``
    (default True -- the miner wants programs not reasoning traces; the bench config flips
    it False so reasoning tail models match the other reasoning entries), ``validate``,
    ``priority``, ``preferred_targets``, ``submitter``, ``max_runtime_seconds``,
    ``progress_idle_timeout_seconds``, ``wait_seconds``, ``poll_timeout_seconds``. A done
    job's ``result`` is a JSON string ``{"content": ..., optional "reasoning": ...}``.
    """
    if not destination.base_url:
        raise RuntimeError("destination base_url (the T'Ra queue) is required for tra_queue")
    base = destination.base_url.rstrip("/")
    eb = destination.extra_body or {}
    payload: dict[str, Any] = {
        "prompt": prompt,
        "max_tokens": _max_tokens_for(destination, options),
        "request_timeout_seconds": destination.request_timeout_seconds,
        # The scheduler kills a job at max_runtime / on idle -- bounds reasoning runaway.
        "max_runtime_seconds": int(eb.get("max_runtime_seconds", destination.request_timeout_seconds)),
        "progress_idle_timeout_seconds": int(eb.get("progress_idle_timeout_seconds", 180)),
        # No-op on models that don't think; default off for the miner (programs, not
        # traces), the bench config flips it on so reasoning tail models think.
        # Defaults to FALSE: a board should measure the model as it is actually
        # served, not a quietly de-tuned version of it. This used to default True,
        # which combined badly with the scheduler's own default -- T'Ra injected
        # chat_template_kwargs={"enable_thinking": False} whenever this was not
        # explicitly False and the model name contained "qwen3" or "ornith". The
        # result was that an A/B of Ornith against llama-server's --reasoning flag
        # ran with thinking suppressed in BOTH arms, and nothing said so. T'Ra now
        # acts only on an explicit true (queue_server.py), and so does this.
        #
        # Set "disable_thinking": true on the destination when you genuinely want
        # programs rather than traces -- the idea-miner is the case that wants it.
        "disable_thinking": bool(eb.get("disable_thinking", False)),
    }
    if destination.model:
        payload["model"] = destination.model
    if destination.temperature is not None and destination.temperature >= 0:
        payload["temperature"] = destination.temperature
    pref = eb.get("preferred_targets", destination.preferred_targets)
    if pref:
        payload["preferred_targets"] = pref
    for key in ("target_name", "target"):
        if key in eb:
            payload[key] = eb[key]

    # Generation params reach the backend only via a NESTED payload["extra_body"],
    # which the queue merges straight into its chat/completions request. Anything
    # set at payload top level other than model/temperature/max_tokens is dropped,
    # which is why a destination carrying "top_p": 0.95 was previously served at
    # the backend's own default.
    #
    # The end-marker stop is OPT-IN here, unlike the chat-completions adapter which
    # sends it by default. It is incompatible with thinking models, and that is not
    # a quirk of one of them -- it is structural. The prompt instructs the model to
    # print __AETHER_BENCH_END__ after the program; a model that reasons first
    # restates its own instructions while reasoning, names the marker, and trips the
    # stop before writing any code. Observed on all three benchmarked so far:
    #   DeepSeek-V4-Flash -- empty replies, every task
    #   GLM-4.7-Flash     -- 559 chars of bulleted analysis, cut at "Constrain..."
    #   Ornith-1.0-35B    -- 269 chars, cut at "end with `"
    # Ornith only looked safe while two other bugs hid it: the scheduler was
    # suppressing its thinking, and this adapter was not forwarding stop at all.
    #
    # The cost of leaving it off is that a model which never emits EOS runs to
    # max_tokens (GLM: program at char 347, then 41,643 characters of the guide
    # recited back). Bound that with max_output_tokens, sized above the longest
    # legitimate program -- not by re-enabling stop, which trades a time problem for
    # a correctness one. Set "stop": [...] explicitly for a non-thinking model.
    backend_extra: dict[str, Any] = {}
    stop_value = eb.get("stop")
    if stop_value is not None:
        backend_extra["stop"] = stop_value
    for key in ("top_p", "top_k", "min_p", "repeat_penalty", "presence_penalty",
                "frequency_penalty", "seed", "chat_template_kwargs"):
        if key in eb:
            backend_extra[key] = eb[key]
    # A destination may also pass a nested extra_body through verbatim; it wins,
    # so an explicit override is always available.
    nested = eb.get("extra_body")
    if isinstance(nested, dict):
        backend_extra.update(nested)
    # The per-repeat seed (destination seed + repeat_index) rides the same nested
    # extra_body, the only path that reaches the backend.
    if options is not None and options.seed is not None:
        backend_extra["seed"] = int(options.seed)
    if backend_extra:
        payload["extra_body"] = backend_extra

    body = {
        "resource_group": "llm",
        "type": "llm_generate",
        "payload": payload,
        "priority": int(eb.get("priority", destination.priority)),
        "submitter": eb.get("submitter", "aether_doc_bench"),
    }
    # Idempotency key. Stable across THIS call's internal submit retries -- which is
    # the orphan trap it exists to prevent -- but distinct across separate logical
    # requests.
    #
    # It was previously md5(destination_id|model|prompt) alone, i.e. stable across
    # everything, which silently replayed stale results three different ways:
    #   * --repeats N returned ONE job N times instead of N independent samples, so
    #     any repeat column measured a single draw duplicated.
    #   * a probe issuing the same prompt twice got one job back, and a control arm
    #     that had errored stayed errored on every subsequent iteration.
    #   * worst: after a SERVER-side config change (llama-server --reasoning off ->
    #     on) the same destination id replayed the pre-change answers. A verification
    #     request came back in 2s with the server log showing no request had arrived;
    #     an entire reasoning-on board would have been a byte-identical copy of the
    #     reasoning-off one, supporting a confident and completely wrong conclusion.
    #
    # The payload fingerprint covers request-side changes (sampling, stop, model,
    # max_tokens). The per-call nonce covers repeats and server-side changes, which
    # nothing in the payload can see. Computed once, before the submit loop, so the
    # retries it guards still share it.
    request_fingerprint = hashlib.md5(
        json.dumps(payload, sort_keys=True, default=str).encode("utf-8")
    ).hexdigest()[:8]
    call_nonce = os.urandom(6).hex()
    idem = ("aether-tra:" + hashlib.md5(
        f"{destination.destination_id}|{destination.model}|{prompt}".encode("utf-8")
    ).hexdigest()[:16] + f":{request_fingerprint}:{call_nonce}")

    # 1) Dry-run (POST /jobs/explain). Bail early ONLY on a hard routing failure (model
    # not servable anywhere); transient busy/lock just means the job will queue.
    if eb.get("validate", True):
        try:
            ex = http_json_request(f"{base}/jobs/explain", body, None, timeout_seconds=30)
            if not ex.get("routable", True):
                note = str(ex.get("scheduler_note") or "")
                # Bail ONLY on a true hard failure: the model is not servable on ANY
                # target (`no_route_for_model`). Every `no_eligible_target:X [<state>=...]`
                # -- busy / loading / external_busy / locked / ... -- is TRANSIENT:
                # submitting queues the job and it runs once a target becomes eligible.
                # Enumerating the transient states is fragile (we missed `loading=` the
                # first pass and false-failed a whole run while LM Studio loaded the
                # model), so treat anything that is not no_route_for_model as queue-able.
                if note.split(":", 1)[0] == "no_route_for_model":
                    raise RuntimeError(f"scheduler will not route this job: {note}")
        except RuntimeError:
            raise
        except Exception:
            pass  # explain is best-effort

    # 2) Submit (idempotent + retried on transient queue-server errors).
    submit = http_json_request(
        f"{base}/jobs", body, None, timeout_seconds=30,
        max_retries=max(2, destination.request_max_retries),
        retry_backoff_seconds=destination.retry_backoff_seconds or 2.0,
        extra_headers={**(destination.extra_headers or {}), "Idempotency-Key": idem},
    )
    job_id = submit.get("id")
    if not job_id:
        raise RuntimeError(f"tra_queue submit returned no job id: {submit}")

    def _cancel() -> None:
        try:
            http_json_request(
                f"{base}/jobs/{job_id}/cancel", {"note": "aether bench gave up"},
                None, timeout_seconds=15,
            )
        except Exception:
            pass

    # 3) Long-poll: GET /jobs/{id}?wait=N blocks server-side up to N seconds. Transient
    # poll errors (refused/dropped/timeout under load) retry the SAME job; on give-up the
    # job is cancelled so it never orphans holding the GPU.
    wait_window = int(eb.get("wait_seconds", 30))
    poll_deadline = time.time() + max(
        int(destination.request_timeout_seconds), int(eb.get("poll_timeout_seconds", 1800))
    )
    consecutive_errors = 0
    last_status = None
    try:
        while time.time() < poll_deadline:
            try:
                req = urllib.request.Request(f"{base}/jobs/{job_id}?wait={wait_window}", method="GET")
                with urllib.request.urlopen(req, timeout=wait_window + 30) as resp:
                    status = json.loads(resp.read().decode("utf-8", "replace"))
                consecutive_errors = 0
            except Exception:  # transient scheduler hiccup -- keep waiting
                consecutive_errors += 1
                if consecutive_errors >= 10:
                    raise RuntimeError(
                        f"tra_queue unreachable for job {job_id} "
                        f"({consecutive_errors} consecutive poll failures)"
                    )
                time.sleep(min(30.0, 3.0 * consecutive_errors))
                continue
            st = (status.get("status") or "").lower()
            if st in ("done", "complete", "completed", "succeeded", "success"):
                raw = status.get("result")
                result = json.loads(raw) if isinstance(raw, str) else (raw or {})
                return {
                    "raw_text": (result or {}).get("content", "") or "",
                    "usage": (result or {}).get("usage") or status.get("usage"),
                    "response_id": job_id,
                    "model": status.get("target_name"),
                    "finish_reason": (result or {}).get("finish_reason") or status.get("finish_reason"),
                }
            if st in ("failed", "error", "canceled", "cancelled", "archived_canceled", "rejected"):
                why = status.get("error") or status.get("display_error") or status.get("scheduler_note") or "unknown"
                raise RuntimeError(f"tra_queue job {job_id} {st}: {why}")
            last_status = st
        raise ProviderTimeoutError(
            f"tra_queue job {job_id} not done within deadline (last status={last_status})"
        )
    except BaseException:
        # Any non-success exit (deadline, persistent poll failure, interrupt) cancels the
        # job so it never orphans in the queue holding the GPU.
        _cancel()
        raise


def invoke_openai_completions(
    prompt: str, destination: Destination, options: RequestOptions | None = None
) -> dict[str, Any]:
    if not destination.model:
        raise RuntimeError("destination model is required for openai_completions")
    base_url = (destination.base_url or "https://api.openai.com/v1").rstrip("/")
    api_key = resolve_api_key(destination)

    body = {
        "model": destination.model,
        "prompt": prompt,
        "max_tokens": _max_tokens_for(destination, options),
        "stop": [OUTPUT_END_MARKER],
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature
    if options is not None and options.seed is not None:
        body["seed"] = int(options.seed)

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
        "finish_reason": choices[0].get("finish_reason"),
    }


def invoke_command(
    prompt: str,
    command_template: str,
    cwd: pathlib.Path,
    timeout_seconds: int,
    options: RequestOptions | None = None,
) -> dict[str, Any]:
    """Run a local model command. The per-request seed and output budget reach it
    as {seed} / {max_tokens} template placeholders and as AETHER_BENCH_SEED /
    AETHER_BENCH_MAX_TOKENS in its environment (empty when unset)."""
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".prompt", delete=False) as handle:
        handle.write(prompt)
        prompt_file = pathlib.Path(handle.name)

    seed_text = "" if options is None or options.seed is None else str(int(options.seed))
    max_tokens_text = "" if options is None or options.max_tokens is None else str(int(options.max_tokens))
    env = dict(os.environ)
    env["AETHER_BENCH_SEED"] = seed_text
    env["AETHER_BENCH_MAX_TOKENS"] = max_tokens_text
    try:
        command = command_template.format(
            prompt_file=str(prompt_file), seed=seed_text, max_tokens=max_tokens_text
        )
        proc = subprocess.run(
            command,
            shell=True,
            cwd=str(cwd),
            text=True,
            errors="replace",
            capture_output=True,
            timeout=timeout_seconds,
            env=env,
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
        if payload.get("finish_reason") is not None:
            result["finish_reason"] = payload.get("finish_reason")
        return result

    return {
        "raw_text": stdout_text,
        "stderr": proc.stderr,
    }


def release_destination_model(destination: Destination) -> dict[str, Any]:
    """Tell the T'Ra queue this run is done with its model, so the queue can
    unload it wherever no running job uses it (POST /api/llm/release). Loading a
    model never unloaded the previous one, and on 2026-10-07 the models a board
    had finished with, plus a co-tenant, ran m5 out of GPU memory. Other kinds
    load nothing through the harness, so they have nothing to release."""
    if destination.kind not in ("tra_queue", "tra_scheduler") or not destination.base_url or not destination.model:
        return {"skipped": f"nothing to release for a {destination.kind} destination"}
    eb = destination.extra_body or {}
    body: dict[str, Any] = {
        "model": destination.model,
        "submitter": eb.get("submitter", "aether_doc_bench"),
    }
    targets = eb.get("preferred_targets", destination.preferred_targets)
    if targets:
        body["targets"] = list(targets)
    try:
        reply = http_json_request(destination.base_url.rstrip("/") + "/api/llm/release", body, None,
                                  timeout_seconds=180, extra_headers=destination.extra_headers)
    except Exception as exc:  # noqa: BLE001 -- releasing is best effort; the run's results stand
        return {"error": f"{type(exc).__name__}: {str(exc)[:300]}"}
    released = [u.get("model") for tgt in reply.get("targets") or [] for u in tgt.get("released") or []]
    print(f"[release] {destination.destination_id} ({destination.model}): unloaded on "
          f"{[tgt.get('target') for tgt in reply.get('targets') or [] if tgt.get('released')]}"
          + ("" if released else " (nothing loaded or still in use)"), file=sys.stderr)
    return reply


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


def preflight_destination(destination: Destination) -> tuple[bool, str]:
    """Confirm a destination can actually answer BEFORE scoring a suite against it.

    Providers keep retired models in their listings, and per-model parameter
    rules differ, so a misconfigured destination produces a full sweep of
    failures that reads exactly like a bad model. Both happened in one session:

      * gemini-2.0-flash-lite is retired and 404s -> 0/12
      * gpt-5-nano/mini reject `stop`, which the chat adapter sends by default,
        so every request 400'd -> 0/41

    Neither measured anything, and both looked like scores. This sends ONE
    trivial prompt through the SAME adapter the benchmark will use -- not a
    hand-built approximation, which is the mistake that let the `stop` rejection
    through in the first place -- so the model's existence AND the exact request
    shape are both validated.

    Returns (ok, detail). A failure should skip the destination rather than
    record N task failures against it.

    `command` destinations are exempt: they are local scripts, not remote
    providers, so neither failure mode applies -- and the deterministic
    self-test fake answers only its own known prompts, so probing it with
    anything else fails by design.
    """
    if destination.kind == "command":
        return True, "skipped (local command destination)"
    try:
        result = run_model_with_deadline(
            "Reply with exactly the word: OK", destination
        )
    except Exception as exc:  # provider error, timeout, bad shape -- all disqualifying
        return False, f"{type(exc).__name__}: {str(exc)[:200]}"
    text = (result.get("raw_text") or "").strip()
    if not text:
        return False, "reply contained no content"
    return True, text[:40]


def run_model(
    prompt: str, destination: Destination, options: RequestOptions | None = None
) -> dict[str, Any]:
    if destination.kind == "openai_responses":
        return invoke_openai_responses(prompt=prompt, destination=destination, options=options)
    if destination.kind == "openai_chat_completions":
        return invoke_openai_chat_completions(prompt=prompt, destination=destination, options=options)
    if destination.kind in ("tra_queue", "tra_scheduler"):
        return invoke_tra_queue(prompt=prompt, destination=destination, options=options)
    if destination.kind == "openai_completions":
        return invoke_openai_completions(prompt=prompt, destination=destination, options=options)
    if destination.kind == "command":
        if not destination.command_template:
            raise RuntimeError("command_template is required for command destinations")
        return invoke_command(
            prompt=prompt,
            command_template=destination.command_template,
            cwd=REPO_ROOT,
            timeout_seconds=max(30, int(destination.request_timeout_seconds)),
            options=options,
        )
    raise RuntimeError(f"unsupported destination type {destination.kind}")


def _run_model_worker(
    prompt: str, destination: Destination, queue: Any, options: RequestOptions | None = None
) -> None:
    try:
        queue.put(("ok", run_model(prompt, destination, options)))
    except Exception as exc:
        queue.put(("err", str(exc)))


def run_model_with_deadline(
    prompt: str, destination: Destination, options: RequestOptions | None = None
) -> dict[str, Any]:
    deadline = max(1, int(destination.request_timeout_seconds))
    ctx = multiprocessing.get_context("spawn")
    queue: Any = ctx.Queue()
    proc = ctx.Process(target=_run_model_worker, args=(prompt, destination, queue, options))
    proc.start()
    # queue.get() must happen before proc.join(): a worker payload larger than
    # the OS pipe buffer (~64KB on macOS) blocks in queue.put() until drained,
    # so joining first (without reading) can deadlock both sides for up to
    # the full deadline instead of returning promptly.
    try:
        status, payload = queue.get(timeout=deadline)
    except queue_module.Empty:
        proc.terminate()
        proc.join(5)
        raise ProviderTimeoutError(f"provider request exceeded {deadline} seconds")
    proc.join(5)
    if proc.is_alive():
        proc.terminate()
        proc.join(5)
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


def aether_flags(args: Any) -> list[str]:
    """Every flag the harness puts in front of the program path on an aether
    call: the sandbox deny list, then each --aether-arg in order."""
    sandbox_flags = ["--deny", args.sandbox_deny] if getattr(args, "sandbox_deny", "") else []
    return [*sandbox_flags, *[str(a) for a in (getattr(args, "aether_args", None) or [])]]


# Per-stream cap on captured program output. A runaway print loop used to be
# buffered whole until the timeout fired; the head is all a report or a repair
# prompt can use, and no expected stdout is anywhere near this size.
OUTPUT_CAP_BYTES = 1_000_000
TIMEOUT_RETURNCODE = 124

# A per-case temp dir the harness creates (aether-doc-bench-*, and the python/
# rust lanes' equivalents), with whatever absolute prefix the platform puts in
# front of it. Stripped from stderr, diagnostics and fingerprints: the path is
# random per case, so leaving it in kept identical failures from clustering,
# showed the model a meaningless path, and leaked the account's temp dir into
# reports.
# The VM's runtime errors print the path without its leading "/", hence the
# match from any token start rather than from a "/".
_RUN_DIR_RE = re.compile(
    r"(?<![^\s:'\"(\[=])[^\s:'\"()\[\]]*?(?:aether|python|rust)-doc-bench-[A-Za-z0-9_]+/"
)


def strip_run_dirs(text: Any) -> Any:
    """`/var/folders/.../aether-doc-bench-x1y2/task.aether:3: ...` -> `task.aether:3: ...`."""
    if not isinstance(text, str) or "-doc-bench-" not in text:
        return text
    return _RUN_DIR_RE.sub("", text)


def _strip_run_dirs_in_diagnostics(diagnostics: Any) -> Any:
    if isinstance(diagnostics, list):
        return [_strip_run_dirs_in_diagnostics(item) for item in diagnostics]
    if isinstance(diagnostics, dict):
        return {key: _strip_run_dirs_in_diagnostics(value) for key, value in diagnostics.items()}
    return strip_run_dirs(diagnostics)


def _decode_output(data: bytes) -> str:
    # Same decoding and newline translation subprocess's text mode applied, so
    # stored stdout stays comparable with every earlier report.
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")


def run_captured(
    cmd: list[str],
    *,
    cwd: pathlib.Path,
    timeout: float,
    input_text: str | None = None,
    cap_bytes: int = OUTPUT_CAP_BYTES,
) -> dict[str, Any]:
    """Run a program to completion or to its timeout, never raising on either.

    A timeout kills the program and comes back as returncode 124 with
    timed_out=True and whatever output arrived before the kill -- it used to
    raise TimeoutExpired, which the case loop filed as a generation error,
    discarding the source and every attempt. stdin is the given text, or empty
    (never the harness's own terminal). Each stream keeps at most cap_bytes;
    *_truncated says whether more was dropped."""
    started = time.time()
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdin=subprocess.PIPE if input_text is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    truncated = {"stdout": False, "stderr": False}

    def drain(stream: Any, key: str) -> None:
        buffer = buffers[key]
        try:
            while True:
                chunk = stream.read(65536)
                if not chunk:
                    break
                room = cap_bytes - len(buffer)
                if room > 0:
                    buffer.extend(chunk[:room])
                if len(chunk) > max(room, 0):
                    truncated[key] = True
        except (OSError, ValueError):
            pass
        finally:
            try:
                stream.close()
            except OSError:
                pass

    readers = [
        threading.Thread(target=drain, args=(proc.stdout, "stdout"), daemon=True),
        threading.Thread(target=drain, args=(proc.stderr, "stderr"), daemon=True),
    ]
    for reader in readers:
        reader.start()
    writer = None
    if input_text is not None:
        def feed() -> None:
            try:
                proc.stdin.write(input_text.encode("utf-8"))
            except (BrokenPipeError, OSError, ValueError):
                pass
            finally:
                try:
                    proc.stdin.close()
                except (BrokenPipeError, OSError, ValueError):
                    pass

        writer = threading.Thread(target=feed, daemon=True)
        writer.start()

    timed_out = False
    try:
        returncode = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        returncode = proc.wait()
    for reader in readers:
        reader.join(timeout=10)
    if writer is not None:
        writer.join(timeout=5)
    result: dict[str, Any] = {
        "returncode": TIMEOUT_RETURNCODE if timed_out else returncode,
        "stdout": _decode_output(bytes(buffers["stdout"])),
        "stderr": _decode_output(bytes(buffers["stderr"])),
        "elapsed_seconds": round(time.time() - started, 3),
        "timed_out": timed_out,
    }
    if timed_out:
        result["timeout_seconds"] = timeout
        result["killed_returncode"] = returncode
    for key in ("stdout", "stderr"):
        if truncated[key]:
            result[f"{key}_truncated"] = True
    return result


def compile_and_run(task: Task, source_code: str, args: argparse.Namespace) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="aether-doc-bench-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        program_path = tmp_dir / f"{task.task_id}.aether"
        work_dir = tmp_dir if task.cwd is None else tmp_dir / task.cwd
        work_dir.mkdir(parents=True, exist_ok=True)
        materialize_task_files(task, tmp_dir)
        program_path.write_text(source_code, encoding="utf-8")

        flags = aether_flags(args)
        cmd = [str(args.aether_bin), *flags, "--no-cache", str(program_path)]
        # What the report records: the binary the user named (not the per-run
        # snapshot under a random temp dir) and the program by file name.
        recorded_cmd = [
            str(getattr(args, "aether_bin_display", None) or args.aether_bin),
            *flags,
            "--no-cache",
            program_path.name,
        ]
        main_run = run_captured(cmd, cwd=work_dir, timeout=task.timeout_seconds, input_text=task_stdin_text(task))

        stdout = main_run["stdout"]
        stderr = strip_run_dirs(main_run["stderr"])
        exact_match = is_exact(task, main_run["returncode"], stdout)
        diagnostics = None
        diagnostics_timed_out = False

        # The --diagnostics-json rerun repeats the whole run, so it is skipped
        # after a timeout (it would only time out again) and is itself bounded.
        if main_run["returncode"] != 0 and not main_run["timed_out"] and not exact_match:
            diag_cmd = [str(args.aether_bin), *flags, "--diagnostics-json", "--no-cache", str(program_path)]
            diag_run = run_captured(diag_cmd, cwd=work_dir, timeout=task.timeout_seconds,
                                    input_text=task_stdin_text(task))
            diagnostics_timed_out = bool(diag_run["timed_out"])
            diag_text = (diag_run["stderr"] or "").strip()
            if diag_text and not diagnostics_timed_out:
                try:
                    diagnostics = _strip_run_dirs_in_diagnostics(json.loads(diag_text))
                except json.JSONDecodeError:
                    diagnostics = None

        result = {
            "command": recorded_cmd,
            "returncode": main_run["returncode"],
            "stdout": stdout,
            "stderr": stderr,
            "diagnostics": diagnostics,
            "elapsed_seconds": main_run["elapsed_seconds"],
            "exact_stdout_match": exact_match,
            "expected_returncode": task.expected_returncode,
            "timed_out": main_run["timed_out"],
            "binary_sha256": getattr(args, "binary_sha256", None),
        }
        for key in ("timeout_seconds", "killed_returncode", "stdout_truncated", "stderr_truncated"):
            if key in main_run:
                result[key] = main_run[key]
        if diagnostics_timed_out:
            result["diagnostics_timed_out"] = True
        return result


def _run_record(cmd: list[str], run: dict[str, Any], task: Task) -> dict[str, Any]:
    record = {
        "command": cmd,
        "returncode": run["returncode"],
        "stdout": run["stdout"],
        "stderr": strip_run_dirs(run["stderr"]),
        "diagnostics": None,
        "elapsed_seconds": run["elapsed_seconds"],
        "exact_stdout_match": is_exact(task, run["returncode"], run["stdout"]),
        "expected_returncode": task.expected_returncode,
        "timed_out": run["timed_out"],
    }
    for key in ("timeout_seconds", "killed_returncode", "stdout_truncated", "stderr_truncated"):
        if key in run:
            record[key] = run[key]
    return record


def run_python_task(task: Task, source_code: str) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="python-doc-bench-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        program_path = tmp_dir / f"{task.task_id}.py"
        work_dir = tmp_dir if task.cwd is None else tmp_dir / task.cwd
        work_dir.mkdir(parents=True, exist_ok=True)
        materialize_task_files(task, tmp_dir)
        program_path.write_text(source_code, encoding="utf-8")

        cmd = ["python3", str(program_path)]
        run = run_captured(cmd, cwd=work_dir, timeout=task.timeout_seconds, input_text=task_stdin_text(task))
        return _run_record(["python3", program_path.name], run, task)


def run_rust_task(task: Task, source_code: str) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="rust-doc-bench-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        program_path = tmp_dir / f"{task.task_id}.rs"
        binary_path = tmp_dir / f"{task.task_id}.bin"
        work_dir = tmp_dir if task.cwd is None else tmp_dir / task.cwd
        work_dir.mkdir(parents=True, exist_ok=True)
        materialize_task_files(task, tmp_dir)
        program_path.write_text(source_code, encoding="utf-8")

        compile_cmd = ["rustc", "-O", "-o", str(binary_path), str(program_path)]
        recorded_compile = ["rustc", "-O", "-o", binary_path.name, program_path.name]
        started = time.time()
        compile_run = run_captured(compile_cmd, cwd=work_dir, timeout=task.timeout_seconds)
        if compile_run["returncode"] != 0:
            record = _run_record(recorded_compile, compile_run, task)
            record["stdout"] = ""
            record["exact_stdout_match"] = False
            return record

        remaining = max(1.0, task.timeout_seconds - (time.time() - started))
        run = run_captured([str(binary_path)], cwd=work_dir, timeout=remaining, input_text=task_stdin_text(task))
        record = _run_record([binary_path.name], run, task)
        record["elapsed_seconds"] = round(time.time() - started, 3)
        return record


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
        elif runner == "rust":
            attempt["run"] = run_rust_task(task, source_code)
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


# The previous program's share of a repair prompt. The old 1,200-character cap
# (shared with stdout/stderr, head only) cut large and hard tasks by 22-47% of
# their lines, often including the line the diagnostic cites. 8,000 covers the
# largest reference (3,501 chars) with room; it is not unbounded because one
# stored "source" was 87K characters of leaked reasoning.
REPAIR_SOURCE_LIMIT_DEFAULT = 8000
REPAIR_FEEDBACK_LIMIT_DEFAULT = 1200
SOURCE_HEAD_CHARS = 2000
SOURCE_TAIL_CHARS = 1000
SOURCE_WINDOW_RADIUS = 20
SOURCE_LINE_CAP = 1000


def collapse_repeated_lines(text: str) -> tuple[str, int]:
    """Collapse each run of identical consecutive lines to one line plus
    ` [repeated N times]`. Returns (text, number of lines removed). A flood of
    1,024 identical warnings used to fill the whole stderr budget and hide the
    coded error behind it."""
    if not text:
        return text, 0
    lines = text.split("\n")
    out: list[str] = []
    removed = 0
    index = 0
    while index < len(lines):
        line = lines[index]
        run_end = index + 1
        while run_end < len(lines) and lines[run_end] == line:
            run_end += 1
        count = run_end - index
        if count > 1 and line.strip():
            out.append(f"{line} [repeated {count} times]")
            removed += count - 1
        else:
            out.extend(lines[index:run_end])
        index = run_end
    return "\n".join(out), removed


def cited_source_line(run: dict[str, Any]) -> int | None:
    """The source line a failure points at: the primary diagnostic's line, else
    the first `:N:` in stderr."""
    diagnostic = primary_error_diagnostic(run.get("diagnostics"))
    if diagnostic:
        line = diagnostic.get("line")
        if isinstance(line, int) and line >= 1:
            return line
    match = re.search(r"\.aether:(\d+):", run.get("stderr") or "")
    if match:
        return int(match.group(1))
    match = re.search(r":(\d+):", run.get("stderr") or "")
    return int(match.group(1)) if match else None


def window_source(
    source: str,
    cap: int,
    cited_line: int | None = None,
    head_chars: int = SOURCE_HEAD_CHARS,
    tail_chars: int = SOURCE_TAIL_CHARS,
    radius: int = SOURCE_WINDOW_RADIUS,
) -> tuple[str, dict[str, Any]]:
    """The previous source as a repair prompt shows it. Whole when it fits in
    `cap`; otherwise the first ~2K characters, +/-20 lines around the cited
    line, and the last ~1K, each gap marked `...[N lines omitted (lines a-b)]...`."""
    meta: dict[str, Any] = {
        "source_truncated": False,
        "source_cap": cap,
        "source_chars": len(source),
        "cited_line": cited_line,
    }
    if len(source) <= cap:
        return source, meta
    lines = source.split("\n")
    total = len(lines)

    def take_from(indices: range, budget: int) -> list[int]:
        chosen: list[int] = []
        used = 0
        for idx in indices:
            used += len(lines[idx]) + 1
            if chosen and used > budget:
                break
            chosen.append(idx)
        return chosen

    head = take_from(range(total), head_chars)
    tail = take_from(range(total - 1, -1, -1, ), tail_chars)

    def render(window_radius: int) -> tuple[str, int]:
        keep = set(head) | set(tail)
        if cited_line is not None and 1 <= cited_line <= total:
            lo = max(0, cited_line - 1 - window_radius)
            hi = min(total, cited_line + window_radius)
            keep.update(range(lo, hi))
        parts: list[str] = []
        omitted = 0
        idx = 0
        while idx < total:
            if idx in keep:
                line = lines[idx]
                if len(line) > SOURCE_LINE_CAP:
                    line = line[:SOURCE_LINE_CAP] + " ...[line truncated]..."
                parts.append(line)
                idx += 1
                continue
            gap_end = idx
            while gap_end < total and gap_end not in keep:
                gap_end += 1
            count = gap_end - idx
            omitted += count
            parts.append(f"...[{count} lines omitted (lines {idx + 1}-{gap_end})]...")
            idx = gap_end
        return "\n".join(parts), omitted

    window_radius = radius
    text, omitted = render(window_radius)
    while len(text) > cap and window_radius > 0:
        window_radius = max(0, window_radius // 2 if window_radius > 1 else 0)
        text, omitted = render(window_radius)
    if len(text) > cap:
        text = text[:cap] + "\n...[truncated]..."
    meta.update({
        "source_truncated": True,
        "source_lines": total,
        "omitted_lines": omitted,
        "window_radius": window_radius,
    })
    return text, meta


def build_repair_feedback(
    source: str,
    run: dict[str, Any],
    *,
    source_limit: int = REPAIR_SOURCE_LIMIT_DEFAULT,
    feedback_limit: int = REPAIR_FEEDBACK_LIMIT_DEFAULT,
) -> dict[str, Any]:
    """The previous source, stdout and stderr exactly as a repair prompt shows
    them, plus what was cut. Shared by the bench, session mode and the idea
    miner so all three repair from the same view of a failure."""
    previous_source, meta = window_source(source or "", source_limit, cited_source_line(run))
    stdout = run.get("stdout") or ""
    stderr, collapsed = collapse_repeated_lines(run.get("stderr") or "")
    meta.update({
        "stderr_collapsed": collapsed > 0,
        "stderr_collapsed_lines": collapsed,
        "stdout_truncated": len(stdout) > feedback_limit,
        "stderr_truncated": len(stderr) > feedback_limit,
        "feedback_limit": feedback_limit,
    })
    return {
        "previous_source": previous_source,
        "observed_stdout": truncate_for_prompt(stdout, feedback_limit),
        "observed_stderr": truncate_for_prompt(stderr, feedback_limit),
        "meta": meta,
    }


def is_warning_record(record: dict[str, Any]) -> bool:
    """A --diagnostics-json record that is a warning. Today rea emits warnings
    as severity "error", code null, message "warning: [PREC-001] ..."; once it
    parses the prefix (W6-15) they carry severity "warning" and the code."""
    if str(record.get("severity") or "").lower() == "warning":
        return True
    return str(record.get("message") or "").lstrip().lower().startswith("warning:")


def primary_error_diagnostic(diagnostics: Any) -> dict[str, Any] | None:
    """The diagnostic that names the failure: the first error-severity record
    that is not a warning, else the first record.

    A PREC-001/ARR-001 warning printed before the real error used to become
    diagnostics[0], so the repair round's failure summary quoted the warning
    and the fingerprint keyed on it. Safe both before and after rea learns to
    parse "warning: [CODE]" (W6-15)."""
    records = [d for d in (diagnostics or []) if isinstance(d, dict)]
    for record in records:
        if str(record.get("severity") or "error").lower() == "error" and not is_warning_record(record):
            return record
    return records[0] if records else None


def count_trailing_newlines(text: str) -> int:
    return len(text) - len(text.rstrip("\n"))


def visible_tail(text: str, width: int = 24) -> str:
    """repr the tail of a string so trailing whitespace is legible in a prompt."""
    if not text:
        return "'' (empty)"
    prefix = "..." if len(text) > width else ""
    return prefix + repr(text[-width:])


def first_differing_line(expected: str, observed: str) -> str:
    """repr the first line that differs, keeping line endings visible."""
    exp_lines = expected.splitlines(keepends=True)
    got_lines = observed.splitlines(keepends=True)
    for index in range(max(len(exp_lines), len(got_lines))):
        exp_line = exp_lines[index] if index < len(exp_lines) else None
        got_line = got_lines[index] if index < len(got_lines) else None
        if exp_line != got_line:
            exp_text = "<no such line>" if exp_line is None else repr(exp_line)
            got_text = "<no such line>" if got_line is None else repr(got_line)
            return f"first difference at line {index + 1}: expected {exp_text}, observed {got_text}"
    return f"line counts differ: expected {len(exp_lines)}, observed {len(got_lines)}"


def append_trailing_newline_note(detail: str, expected_stdout: str, observed_stdout: str) -> str:
    """Note a trailing-newline delta riding along with a content mismatch.

    The token and diff branches both work on text with trailing newlines
    stripped, so without this the delta stays invisible until the content is
    fixed and the whitespace-only branch finally sees it a round later.
    """
    exp_count = count_trailing_newlines(expected_stdout)
    got_count = count_trailing_newlines(observed_stdout)
    if exp_count == got_count:
        return detail
    separator = "\n" if "\n" in detail else "; "
    return detail + (
        f"{separator}(also: trailing newlines differ — expected {exp_count}, observed {got_count})"
    )


def describe_stdout_mismatch(expected_stdout: str, observed_stdout: str) -> str:
    # Whitespace-only differences must be named explicitly before anything
    # strips or tokenizes the text: both sides render identically in a repair
    # prompt, and a unified diff of the stripped strings comes back empty. A
    # stray trailing println("") used to yield a bare "stdout_mismatch", which
    # gave the repair round nothing at all to act on.
    if expected_stdout.rstrip("\n") == observed_stdout.rstrip("\n") and expected_stdout != observed_stdout:
        exp_count = count_trailing_newlines(expected_stdout)
        got_count = count_trailing_newlines(observed_stdout)
        if got_count > exp_count:
            fix = (
                f"delete {got_count - exp_count} trailing newline(s) — most likely a stray "
                'final print/println("") emitting an extra blank line'
            )
        else:
            fix = f"emit {exp_count - got_count} more trailing newline(s) at the end of the output"
        return (
            "stdout_mismatch: every character matches except TRAILING NEWLINES. "
            f"Expected ends with {exp_count} newline character(s), observed ends with {got_count}. "
            f"expected tail={visible_tail(expected_stdout)} "
            f"observed tail={visible_tail(observed_stdout)}. "
            f"Fix: {fix}."
        )

    if expected_stdout.split() == observed_stdout.split() and expected_stdout != observed_stdout:
        return (
            "stdout_mismatch: the non-whitespace content is identical; the ONLY differences are "
            "WHITESPACE (trailing spaces, blank lines, indentation, or line endings). "
            f"{first_differing_line(expected_stdout, observed_stdout)}. "
            f"expected tail={visible_tail(expected_stdout)} "
            f"observed tail={visible_tail(observed_stdout)}. "
            "Fix: match the expected spacing exactly."
        )

    expected = expected_stdout.rstrip("\n")
    observed = observed_stdout.rstrip("\n")
    exp_tokens = re.split(r"[,\s]+", expected.strip())
    got_tokens = re.split(r"[,\s]+", observed.strip())
    if "\n" not in expected and "\n" not in observed and exp_tokens and got_tokens:
        missing = [t for t in exp_tokens if t not in got_tokens]
        extra = [t for t in got_tokens if t not in exp_tokens]
        detail = f"stdout_mismatch: expected {len(exp_tokens)} token(s), got {len(got_tokens)}"
        if missing:
            detail += f"; missing: {','.join(missing[:20])}"
        if extra:
            detail += f"; unexpected: {','.join(extra[:20])}"
        if not missing and not extra:
            detail += "; same tokens, different order/positions"
        return append_trailing_newline_note(detail, expected_stdout, observed_stdout)
    diff_lines = list(
        difflib.unified_diff(
            expected.splitlines(),
            observed.splitlines(),
            fromfile="expected",
            tofile="observed",
            lineterm="",
        )
    )
    if diff_lines:
        detail = "stdout_mismatch:\n" + "\n".join(diff_lines[:40])
        return append_trailing_newline_note(detail, expected_stdout, observed_stdout)
    return "stdout_mismatch"


def derive_failure_summary(
    generated_ok: bool,
    run: dict[str, Any],
    generation_error: str | None = None,
    expected_stdout: str | None = None,
) -> str:
    if not generated_ok:
        if generation_error:
            first_line = strip_run_dirs(generation_error.strip().splitlines()[0])
            return f"generation_error: {first_line}"
        return "generation_error: empty model output"
    if run.get("timed_out"):
        limit = run.get("timeout_seconds")
        limit_text = f"{limit:g} s" if isinstance(limit, (int, float)) else "its time limit"
        return (
            f"timeout: your program exceeded {limit_text} and was killed before it finished "
            "(an infinite loop, or work far too slow for the input). Any output it printed "
            "before the kill is shown under Observed stdout."
        )
    expected_rc = int(run.get("expected_returncode", 0) or 0)
    if run["returncode"] != expected_rc and run["returncode"] == 0:
        detail = f"exit_status_mismatch: the task requires exit status {expected_rc}, the program exited 0"
        if expected_stdout is not None and run.get("stdout", "") != expected_stdout:
            detail += "; also " + describe_stdout_mismatch(expected_stdout, run.get("stdout", ""))
        return detail
    if run["returncode"] != expected_rc:
        suffix = f" (the task expects exit status {expected_rc})" if expected_rc else ""
        diagnostics = run.get("diagnostics") or []
        if diagnostics:
            first = primary_error_diagnostic(diagnostics) or {}
            code = first.get("code")
            message = (first.get("message") or "").strip()
            if code and message:
                return f"{code}: {message}{suffix}"
            if code:
                return code + suffix
            if message:
                return message + suffix
        stderr = (run.get("stderr") or "").strip()
        if stderr:
            return stderr.splitlines()[0] + suffix
        return f"nonzero_exit:{run['returncode']}{suffix}"
    if expected_stdout is not None:
        return describe_stdout_mismatch(expected_stdout, run.get("stdout", ""))
    return "stdout_mismatch"


def derive_failure_fingerprint(result: dict[str, Any], task_id: str | None = None) -> str:
    """A compact failure class for clustering. Never carries a run's temp path."""
    return strip_run_dirs(_derive_failure_fingerprint(result, task_id))


def _derive_failure_fingerprint(result: dict[str, Any], task_id: str | None = None) -> str:
    if not result.get("generated_ok", False):
        err = result.get("generation_error") or "empty model output"
        return "generation:" + strip_run_dirs(err.strip().splitlines()[0])[:160]
    run = result["run"]
    if run.get("timed_out"):
        return f"timeout:{task_id or result.get('task_id') or 'unknown'}"
    expected_rc = int(run.get("expected_returncode", 0) or 0)
    if run["returncode"] == expected_rc:
        return "stdout_mismatch"
    if run["returncode"] == 0:
        return "exit_status_mismatch"
    if run["returncode"] != 0:
        diagnostics = run.get("diagnostics") or []
        if diagnostics:
            first = primary_error_diagnostic(diagnostics) or {}
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
    options: RequestOptions | None = None,
) -> dict[str, Any]:
    prompt_tokens = approx_tokens(prompt)
    options, fit = fit_request(
        prompt,
        destination,
        options,
        margin=getattr(args, "context_margin", CONTEXT_MARGIN_TOKENS),
        min_output=getattr(args, "min_output_tokens", MIN_OUTPUT_TOKENS),
    )
    attempt: dict[str, Any] = {
        "prompt_kind": prompt_kind,
        "prompt_approx_tokens": prompt_tokens,
        "prompt_tokens_measured": fit["prompt_tokens_measured"],
        "runner": runner,
        "prompt_sha256": sha256_text(prompt),
        "request": {
            "seed": options.seed,
            "max_tokens": _max_tokens_for(destination, options),
        },
        "context_fit": fit,
    }
    generation = run_model_with_deadline(prompt, destination, options)
    source_code = sanitize_code(generation["raw_text"])
    attempt["generation"] = generation
    attempt["finish_reason"] = generation.get("finish_reason")
    attempt["usage"] = normalize_usage(generation.get("usage"))
    record_prompt_token_gap(attempt, destination)
    attempt["generated_ok"] = bool(source_code.strip())
    attempt["source_code"] = source_code
    attempt["source_approx_tokens"] = approx_tokens(source_code) if source_code.strip() else 0
    if attempt["generated_ok"]:
        if runner == "python":
            attempt["run"] = run_python_task(task, source_code)
        elif runner == "rust":
            attempt["run"] = run_rust_task(task, source_code)
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
        # An empty reply is a serving problem (stop marker, output budget,
        # template), not a verdict on the guide: measured nothing, re-run it.
        attempt["infra_failed"] = True
        attempt["infra_kind"] = "empty_output"
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


# Generation failures that measured nothing: re-run them (rerun_nogen_cases.py),
# never score them and never drop them from the denominator (D37d).
_INFRA_PATTERNS: tuple[tuple[str, str], ...] = (
    ("insufficient_quota", "quota"),
    ("resource_exhausted", "quota"),
    ("http api error 402", "quota"),
    ("http api error 429", "rate_limited"),
    ("rate limit", "rate_limited"),
    ("http api error 4", "http_4xx"),
    ("http api error 5", "http_5xx"),
    ("http api request timed out", "provider_timeout"),
    ("provider request exceeded", "provider_timeout"),
    ("not done within deadline", "provider_timeout"),
    ("http api request failed", "transport"),
    ("tra_queue unreachable", "transport"),
    ("connection refused", "transport"),
    ("connection reset", "transport"),
    ("reset by peer", "transport"),
    ("hit max_tokens", "output_budget"),
    ("did not contain", "malformed_reply"),
    ("tra_queue job", "provider_error"),
    ("scheduler will not route", "provider_error"),
    ("command provider failed", "provider_error"),
    ("empty model output", "empty_output"),
    ("shared batch did not return", "empty_output"),
)


def classify_infra_failure(message: str | None) -> str | None:
    """The infra kind of a generation error message, or None if it is not one."""
    text = (message or "").lower()
    if not text:
        return None
    for needle, kind in _INFRA_PATTERNS:
        if needle in text:
            return kind
    return None


_CODE_RE = re.compile(r"\b[A-Z]+-\d{3}\b")
FAILURE_CLASSES = ("pass", "silent_wrong", "crash_hang", "uncoded_error", "coded_error", "infra_failed", "not_sent")


def classify_attempt(attempt: dict[str, Any]) -> str:
    """The thesis failure class of one attempt, worst first: silent_wrong (the
    program exited as a success but printed the wrong thing), crash_hang
    (timeout, signal, rc >= 128), uncoded_error (failed with no CODE-NNN),
    coded_error -- plus pass, and the two that measured nothing:
    infra_failed and not_sent (context overflow)."""
    if attempt.get("not_sent") or "prompt_too_large" in str(attempt.get("generation_error") or ""):
        return "not_sent"
    run = attempt.get("run") or {}
    if attempt.get("infra_failed") or not attempt.get("generated_ok", bool(attempt.get("source_code"))):
        return "infra_failed"
    if run.get("exact_stdout_match"):
        return "pass"
    returncode = run.get("returncode", -1)
    expected = run.get("expected_returncode", 0)
    if run.get("timed_out") or (isinstance(returncode, int) and (returncode >= 128 or returncode < 0)):
        return "crash_hang"
    if returncode == expected:
        return "silent_wrong"
    codes = [d.get("code") for d in (run.get("diagnostics") or []) if isinstance(d, dict) and d.get("code")]
    if codes or _CODE_RE.search(run.get("stderr") or ""):
        return "coded_error"
    return "uncoded_error"


def first_attempt_of(case: dict[str, Any]) -> dict[str, Any]:
    attempts = case.get("attempts") or []
    return attempts[0] if attempts else case


def case_is_infra_failed(case: dict[str, Any]) -> bool:
    if case.get("infra_failed"):
        return True
    if any(a.get("infra_failed") for a in case.get("attempts") or []):
        return True
    # Records written before infra tagging: a generation error that classifies.
    if not case.get("generated_ok", True) and not (case.get("attempts") or []):
        return classify_attempt(case) == "infra_failed"
    return False


def wilson_interval(successes: int, total: int, z: float = 1.96) -> list[float] | None:
    """Wilson score 95% interval for a binomial rate, as [low, high]. Case-level:
    repeats of one task are not independent, so read it as a lower bound on the
    real uncertainty."""
    if total <= 0:
        return None
    p = successes / total
    denom = 1 + z * z / total
    center = (p + z * z / (2 * total)) / denom
    half = z * ((p * (1 - p) / total + z * z / (4 * total * total)) ** 0.5) / denom
    return [round(max(0.0, center - half), 4), round(min(1.0, center + half), 4)]


def summarize(results: list[dict[str, Any]]) -> dict[str, Any]:
    total = len(results)
    generated = sum(1 for item in results if item["generated_ok"])
    compiled_and_ran = sum(1 for item in results if item["run"]["returncode"] == 0)
    exact = sum(1 for item in results if item["run"]["exact_stdout_match"])
    repaired = sum(1 for item in results if item.get("resolved_after_repair", False))

    classes = {name: 0 for name in FAILURE_CLASSES}
    infra_cases: list[dict[str, Any]] = []
    per_task: dict[str, dict[str, Any]] = {}
    first_exact = 0
    for item in results:
        first = first_attempt_of(item)
        first_class = classify_attempt(first)
        classes[first_class] += 1
        fa_ok = first_class == "pass"
        first_exact += int(fa_ok)
        if case_is_infra_failed(item):
            infra_cases.append({
                "task_id": item.get("task_id"),
                "repeat_index": item.get("repeat_index"),
                "kind": item.get("infra_kind") or first.get("infra_kind")
                or classify_infra_failure(item.get("generation_error")),
            })
        entry = per_task.setdefault(item.get("task_id", ""), {"repeats": 0, "fa_passes": 0, "fx_passes": 0})
        entry["repeats"] += 1
        entry["fa_passes"] += int(fa_ok)
        entry["fx_passes"] += int(bool(item["run"]["exact_stdout_match"]))

    tasks_total = len(per_task)
    fa_majority = sum(1 for e in per_task.values() if e["fa_passes"] * 2 > e["repeats"])
    fx_majority = sum(1 for e in per_task.values() if e["fx_passes"] * 2 > e["repeats"])
    flaky_fa = sorted(t for t, e in per_task.items() if 0 < e["fa_passes"] < e["repeats"])
    flaky_fx = sorted(t for t, e in per_task.items() if 0 < e["fx_passes"] < e["repeats"])
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
        # FA / FX: first-attempt and final exact, with Wilson 95% intervals.
        # infra_failed cases stay in the denominator (dropping them would break
        # the pairing between variants); headline_ok is False while any remain.
        "first_attempt_exact": first_exact,
        "fa_rate": round(first_exact / total, 4) if total else 0.0,
        "fa_ci95": wilson_interval(first_exact, total),
        "final_exact": exact,
        "fx_rate": round(exact / total, 4) if total else 0.0,
        "fx_ci95": wilson_interval(exact, total),
        "first_attempt_classes": classes,
        "infra_failed": len(infra_cases),
        "infra_failed_cases": infra_cases,
        "headline_ok": not infra_cases,
        "tasks": tasks_total,
        "task_majority_fa": fa_majority,
        "task_majority_fx": fx_majority,
        "flaky_fa": flaky_fa,
        "flaky_fx": flaky_fx,
        "per_task": per_task,
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
            summary = variant["summary"]
            if "fa_rate" in summary and summary.get("total_cases"):
                print("       " + headline_line(summary))
                classes = summary.get("first_attempt_classes") or {}
                print("       first-attempt classes: " + "  ".join(
                    f"{name}={classes.get(name, 0)}" for name in FAILURE_CLASSES if name != "pass"))
                if summary.get("flaky_fa") or summary.get("flaky_fx"):
                    print(f"       flaky: FA {', '.join(summary.get('flaky_fa') or []) or '-'}; "
                          f"FX {', '.join(summary.get('flaky_fx') or []) or '-'}")
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

            rust_summary = variant.get("rust_baseline_summary") or {}
            rust_usage = variant.get("rust_baseline_usage_summary") or {}
            rust_source_tokens = variant.get("rust_baseline_source_token_summary") or {}
            rust_final_source_tokens = variant.get("rust_baseline_final_source_token_summary") or {}
            rust_exact_final_source_tokens = variant.get("rust_baseline_exact_final_source_token_summary") or {}
            if rust_summary:
                rs_bits: list[str] = []
                if rust_usage.get("prompt_tokens_total") is not None:
                    rs_bits.append(f"workflow_prompt_tok={rust_usage['prompt_tokens_total']}")
                if rust_usage.get("completion_tokens_total") is not None:
                    rs_bits.append(f"workflow_completion_tok={rust_usage['completion_tokens_total']}")
                if rust_usage.get("total_tokens_total") is not None:
                    rs_bits.append(f"workflow_total_tok={rust_usage['total_tokens_total']}")
                if rust_source_tokens.get("source_approx_tokens_total") is not None:
                    rs_bits.append(f"all_attempt_answer_tok~={rust_source_tokens['source_approx_tokens_total']}")
                if rust_final_source_tokens.get("source_approx_tokens_total") is not None:
                    rs_bits.append(f"final_answer_tok~={rust_final_source_tokens['source_approx_tokens_total']}")
                if rust_exact_final_source_tokens.get("source_approx_tokens_total") is not None:
                    rs_bits.append(f"exact_final_answer_tok~={rust_exact_final_source_tokens['source_approx_tokens_total']}")
                print(
                    f"       rust  : generated={rust_summary['generated_ok']}/{rust_summary['total_cases']}  "
                    f"run={rust_summary['run_ok']}/{rust_summary['total_cases']}  "
                    f"exact={rust_summary['exact_stdout_match']}/{rust_summary['total_cases']}  "
                    f"repaired={rust_summary['resolved_after_repair']}/{rust_summary['total_cases']}"
                )
                if rs_bits:
                    print(f"       rs use: {'  '.join(rs_bits)}")
            for failure in variant.get("failure_patterns", [])[:3]:
                print(
                    f"       fail x{failure['count']}: {failure['fingerprint']} "
                    f"[tasks: {', '.join(failure['task_ids'])}]"
                )
        print("")


def headline_line(summary: dict[str, Any]) -> str:
    """FA and FX with their intervals -- or, while infra failures remain, why
    there is no headline."""
    total = summary.get("total_cases", 0)
    if summary.get("infra_failed"):
        return (f"HEADLINE WITHHELD: {summary['infra_failed']}/{total} case(s) failed in the "
                "infrastructure (re-run them: Tests/aether_doc_bench/rerun_nogen_cases.py --report ...)")

    def fmt(count: int, ci: Any) -> str:
        interval = f" [{ci[0] * 100:.1f}-{ci[1] * 100:.1f}%]" if ci else ""
        return f"{count}/{total} ({(count / total * 100) if total else 0:.1f}%{interval})"

    return (f"FA {fmt(summary['first_attempt_exact'], summary.get('fa_ci95'))}  "
            f"FX {fmt(summary['final_exact'], summary.get('fx_ci95'))}  "
            f"task-majority FA {summary.get('task_majority_fa')}/{summary.get('tasks')} "
            f"FX {summary.get('task_majority_fx')}/{summary.get('tasks')}")


def report_infra_failed(report: dict[str, Any]) -> int:
    count = 0
    for destination in report.get("destinations", []):
        for variant in destination.get("variants", []):
            for key in ("results", "python_baseline_results", "rust_baseline_results"):
                count += sum(1 for case in variant.get(key, []) if case_is_infra_failed(case))
    return count


# --resume: what must match between a checkpointed report and this run for its
# finished cases to count. Anything that changes what a model is asked, or how
# its answer is scored, is here. The destinations file's sha is not: an endpoint
# address can move between hosts mid-board, and each destination is matched by
# id and model instead.
RESUME_REPORT_KEYS = ("tasks_sha256", "binary_sha256", "aether_version")
RESUME_RUN_CONFIG_KEYS = (
    "docs", "repeats", "start_repeat", "seed_base", "repair_attempts",
    "repair_feedback_limit", "repair_source_limit", "context_margin",
    "min_output_tokens", "shared_guide_batch_size", "python_baseline",
    "rust_baseline", "skip_aether", "task_ids",
)
CASE_KEYS = ("results", "python_baseline_results", "rust_baseline_results")


def resume_mismatches(prior: dict[str, Any], report: dict[str, Any],
                      destination_models: dict[str, str]) -> list[str]:
    """Why a checkpointed report cannot be resumed by this run (empty: it can)."""
    problems = []
    for key in RESUME_REPORT_KEYS:
        if prior.get(key) != report.get(key):
            problems.append(f"{key}: the report has {prior.get(key)!r}, this run {report.get(key)!r}")
    prior_guides = prior.get("guides") or {}
    for name, record in (report.get("guides") or {}).items():
        before = (prior_guides.get(name) or {}).get("sha256")
        if before != record.get("sha256"):
            problems.append(f"guide {name}: the report has sha256 {before}, this run {record.get('sha256')}")
    prior_config = prior.get("run_config") or {}
    for key in RESUME_RUN_CONFIG_KEYS:
        if prior_config.get(key) != report["run_config"].get(key):
            problems.append(f"run_config.{key}: the report has {prior_config.get(key)!r}, "
                            f"this run {report['run_config'].get(key)!r}")
    for dest in prior.get("destinations") or []:
        dest_id = dest.get("destination_id")
        if dest_id in destination_models and dest.get("model") != destination_models[dest_id]:
            problems.append(f"destination {dest_id}: the report ran model {dest.get('model')!r}, "
                            f"this run {destination_models[dest_id]!r}")
    return problems


def resume_kept_cases(prior: dict[str, Any]) -> tuple[dict[tuple[str, str, str], list[dict[str, Any]]], int, int]:
    """The checkpointed cases a resumed run keeps, keyed by (destination id, doc
    name, case key), with (kept, dropped) counts. A case that failed in the
    infrastructure measured nothing, so it is dropped and runs again."""
    kept: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    n_kept = n_dropped = 0
    for dest in prior.get("destinations") or []:
        for variant in dest.get("variants") or []:
            for key in CASE_KEYS:
                for case in variant.get(key) or []:
                    if case_is_infra_failed(case):
                        n_dropped += 1
                        continue
                    kept.setdefault((dest["destination_id"], variant["doc_name"], key), []).append(case)
                    n_kept += 1
    return kept, n_kept, n_dropped


def refresh_variant_summaries(variant_report: dict[str, Any]) -> None:
    """Recompute every summary block of a variant from its results."""
    for prefix, key in (("", "results"), ("python_baseline_", "python_baseline_results"),
                        ("rust_baseline_", "rust_baseline_results")):
        if key not in variant_report:
            continue
        results = variant_report[key]
        variant_report[f"{prefix}summary"] = summarize(results)
        variant_report[f"{prefix}usage_summary"] = summarize_usage(results)
        variant_report[f"{prefix}source_token_summary"] = summarize_source_tokens(results)
        variant_report[f"{prefix}final_usage_summary"] = summarize_final_usage(results, "all")
        variant_report[f"{prefix}run_ok_final_usage_summary"] = summarize_final_usage(results, "run_ok")
        variant_report[f"{prefix}exact_final_usage_summary"] = summarize_final_usage(results, "exact")
        variant_report[f"{prefix}final_source_token_summary"] = summarize_final_source_tokens(results, "all")
        variant_report[f"{prefix}run_ok_final_source_token_summary"] = summarize_final_source_tokens(results, "run_ok")
        variant_report[f"{prefix}exact_final_source_token_summary"] = summarize_final_source_tokens(results, "exact")
        patterns_key = "failure_patterns" if not prefix else f"{prefix.split('_')[0]}_failure_patterns"
        variant_report[patterns_key] = summarize_failure_patterns(results)


def resummarize_report(path: pathlib.Path, write: bool = False) -> int:
    """Recompute the summaries of an existing report with today's summarize()
    (FA/FX, classes, CIs) and print them. Exit 3 while infra failures remain."""
    report = load_report_json(path)
    for destination in report.get("destinations", []):
        for variant in destination.get("variants", []):
            refresh_variant_summaries(variant)
    if write:
        write_json_atomic(path, report)
    report.setdefault("tasks_file", str(path))
    report.setdefault("summary", {"total_cases_per_destination": "?", "destination_count": len(report.get("destinations", []))})
    print_text_summary(report)
    return 3 if report_infra_failed(report) else 0


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


def finalize_case_record(attempts: list[dict[str, Any]], task_id: str | None = None) -> dict[str, Any]:
    final_attempt = attempts[-1]
    case_record = dict(final_attempt)
    case_record["attempts"] = attempts
    case_record["attempt_count"] = len(attempts)
    case_record["resolved_after_repair"] = (
        case_record["run"]["exact_stdout_match"] and len(attempts) > 1
    )
    case_record["failure_fingerprint"] = (
        "" if case_record["run"]["exact_stdout_match"] else derive_failure_fingerprint(case_record, task_id)
    )
    infra = [a for a in attempts if a.get("infra_failed")]
    case_record["infra_failed"] = bool(infra)
    if infra:
        case_record["infra_kind"] = infra[0].get("infra_kind")
        case_record["infra_stage"] = infra[0].get("prompt_kind")
    else:
        case_record.pop("infra_kind", None)
    return case_record


def failed_generation_attempt(
    prompt_kind: str,
    runner: str,
    exc: BaseException,
    options: RequestOptions | None = None,
) -> dict[str, Any]:
    """The attempt record for a generation that raised (provider error, deadline,
    malformed reply). Kept in the case's attempts so the rounds already measured
    survive, instead of the whole case collapsing to attempts=[]."""
    message = strip_run_dirs(str(exc)) or type(exc).__name__
    extra: dict[str, Any] = {}
    if isinstance(exc, ContextOverflowError):
        # Deterministic, not a provider event: the request was never sent.
        extra["not_sent"] = "context_overflow"
    else:
        extra["infra_failed"] = True
        extra["infra_kind"] = classify_infra_failure(message) or "provider_error"
    return {
        **extra,
        "prompt_kind": prompt_kind,
        "runner": runner,
        "request": {"seed": None if options is None else options.seed},
        "generated_ok": False,
        "generation_error": message,
        "source_code": "",
        "source_approx_tokens": 0,
        "run": {
            "returncode": -1,
            "stdout": "",
            "stderr": message,
            "elapsed_seconds": 0.0,
            "exact_stdout_match": False,
        },
    }


def _record_feedback(attempt: dict[str, Any], meta: dict[str, Any]) -> None:
    """What this repair round was shown of the previous attempt."""
    attempt["repair_feedback"] = meta
    attempt["source_truncated"] = bool(meta.get("source_truncated"))
    attempt["source_cap"] = meta.get("source_cap")
    attempt["stderr_collapsed"] = bool(meta.get("stderr_collapsed"))


def apply_repairs(
    *,
    initial_attempt: dict[str, Any],
    destination: Destination,
    task: Task,
    args: argparse.Namespace,
    runner: str,
    repair_prompt_builder: Any,
    options: RequestOptions | None = None,
) -> list[dict[str, Any]]:
    attempts: list[dict[str, Any]] = [initial_attempt]
    attempt = initial_attempt

    if args.repair_attempts > 0 and not attempt["run"]["exact_stdout_match"]:
        for repair_index in range(args.repair_attempts):
            failure_summary = derive_failure_summary(
                generated_ok=attempt.get("generated_ok", False),
                run=attempt["run"],
                generation_error=attempt.get("generation_error"),
                expected_stdout=task.expected_stdout,
            )
            feedback = build_repair_feedback(
                attempt.get("source_code", ""),
                attempt["run"],
                source_limit=getattr(args, "repair_source_limit", REPAIR_SOURCE_LIMIT_DEFAULT),
                feedback_limit=args.repair_feedback_limit,
            )
            repair_prompt = repair_prompt_builder(
                task=task,
                previous_source=feedback["previous_source"],
                attempt_number=repair_index + 1,
                failure_summary=failure_summary,
                observed_stdout=feedback["observed_stdout"],
                observed_stderr=feedback["observed_stderr"],
            )
            try:
                attempt = evaluate_attempt(
                    prompt=repair_prompt,
                    prompt_kind="repair",
                    destination=destination,
                    task=task,
                    args=args,
                    runner=runner,
                    options=options,
                )
            except Exception as exc:  # keep the rounds already measured
                attempt = failed_generation_attempt("repair", runner, exc, options)
                _record_feedback(attempt, feedback["meta"])
                attempts.append(attempt)
                break
            _record_feedback(attempt, feedback["meta"])
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
    options: RequestOptions | None = None,
) -> dict[str, Any]:
    try:
        attempt = evaluate_attempt(
            prompt=initial_prompt,
            prompt_kind="initial",
            destination=destination,
            task=task,
            args=args,
            runner=runner,
            options=options,
        )
    except Exception as exc:
        return finalize_case_record([failed_generation_attempt("initial", runner, exc, options)], task.task_id)
    attempts = apply_repairs(
        initial_attempt=attempt,
        destination=destination,
        task=task,
        args=args,
        runner=runner,
        repair_prompt_builder=repair_prompt_builder,
        options=options,
    )
    return finalize_case_record(attempts, task.task_id)


def chunk_list(items: list[Any], size: int) -> list[list[Any]]:
    if size <= 1:
        return [[item] for item in items]
    return [items[idx:idx + size] for idx in range(0, len(items), size)]


def run_single_aether_task(
    *,
    destination: Destination,
    doc_name: str,
    doc_text: str,
    task: Task,
    repeat_index: int,
    args: argparse.Namespace,
    doc_token_reference: dict[str, Any],
    options: RequestOptions | None = None,
) -> dict[str, Any]:
    """One Aether case: the initial prompt, its repair rounds, and the record."""
    if args.progress:
        print_progress_start(destination, doc_name, task, repeat_index)
    prompt = build_prompt(doc_name=doc_name, doc_text=doc_text, task=task)
    case_record: dict[str, Any] = {
        "task_id": task.task_id,
        "task_title": task.title,
        "repeat_index": repeat_index,
        "seed": None if options is None else options.seed,
        "attempts": [],
        "doc_token_reference": doc_token_reference,
    }
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
            options=options,
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
        case_record["failure_fingerprint"] = derive_failure_fingerprint(case_record, task.task_id)
    finally:
        try:
            run_destination_cleanup(destination, task, doc_name, repeat_index)
        except Exception as cleanup_exc:  # pragma: no cover - surfaced in JSON report
            case_record["cleanup_error"] = str(cleanup_exc)
    if args.progress:
        print_progress_done(destination, doc_name, task, repeat_index, case_record)
    return case_record


def run_aether_task_group(
    *,
    destination: Destination,
    doc_name: str,
    doc_text: str,
    task_group: list[Task],
    repeat_index: int,
    args: argparse.Namespace,
    doc_token_reference: dict[str, Any],
    options: RequestOptions | None = None,
    on_case_complete: Any | None = None,
) -> tuple[list[dict[str, Any]], dict[str, Any] | None]:
    """Run one unit of work under one guide variant: a single task, or one
    shared-guide batch of tasks. Returns (case records, batch metadata or None)."""
    results: list[dict[str, Any]] = []

    def emit(case_record: dict[str, Any]) -> None:
        results.append(case_record)
        if on_case_complete:
            on_case_complete(case_record)

    def single(task: Task) -> dict[str, Any]:
        return run_single_aether_task(
            destination=destination,
            doc_name=doc_name,
            doc_text=doc_text,
            task=task,
            repeat_index=repeat_index,
            args=args,
            doc_token_reference=doc_token_reference,
            options=options,
        )

    if len(task_group) <= 1 or effective_shared_guide_batch_size(args, destination) <= 1:
        for task in task_group:
            emit(single(task))
        return results, None

    for task in task_group:
        if args.progress:
            print_progress_start(destination, doc_name, task, repeat_index)

    batch_prompt = build_batch_prompt(doc_name=doc_name, doc_text=doc_text, tasks=task_group)
    try:
        batch_options, batch_fit = fit_request(
            batch_prompt, destination, options,
            margin=getattr(args, "context_margin", CONTEXT_MARGIN_TOKENS),
            min_output=getattr(args, "min_output_tokens", MIN_OUTPUT_TOKENS),
        )
    except ContextOverflowError:
        for task in task_group:
            emit(single(task))
        return results, None

    batch_prompt_tokens = approx_tokens(batch_prompt)
    batch_id = f"{doc_name}-r{repeat_index}-{'-'.join(task.task_id for task in task_group)}"
    batch_meta: dict[str, Any] = {
        "batch_id": batch_id,
        "repeat_index": repeat_index,
        "task_ids": [task.task_id for task in task_group],
        "prompt_approx_tokens": batch_prompt_tokens,
        "doc_name": doc_name,
        "context_fit": batch_fit,
    }

    try:
        shared_generation = run_model_with_deadline(batch_prompt, destination, batch_options)
        shared_usage = normalize_usage(shared_generation.get("usage"))
        split_usage = split_usage_across_tasks(shared_usage, len(task_group))
        split_prompt_tokens = split_int_total(batch_prompt_tokens, len(task_group))
        sources = parse_batch_sources(
            shared_generation.get("raw_text", ""),
            [task.task_id for task in task_group],
        )
        batch_meta["usage"] = shared_usage
        batch_meta["parsed_task_count"] = len(sources)
    except Exception as exc:  # pragma: no cover - surfaced in JSON report
        batch_meta["error"] = str(exc)
        for task in task_group:
            case_record = single(task)
            case_record["batch_fallback_reason"] = str(exc)
            emit(case_record)
        return results, batch_meta

    for idx, task in enumerate(task_group):
        case_record: dict[str, Any] = {
            "task_id": task.task_id,
            "task_title": task.title,
            "repeat_index": repeat_index,
            "seed": None if options is None else options.seed,
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
                options=options,
            )
            case_record.update(finalize_case_record(attempts, task.task_id))
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
            case_record["failure_fingerprint"] = derive_failure_fingerprint(case_record, task.task_id)
        finally:
            try:
                run_destination_cleanup(destination, task, doc_name, repeat_index)
            except Exception as cleanup_exc:  # pragma: no cover - surfaced in JSON report
                case_record["cleanup_error"] = str(cleanup_exc)
        if args.progress:
            print_progress_done(destination, doc_name, task, repeat_index, case_record)
        emit(case_record)

    return results, batch_meta


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
    options: RequestOptions | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Every task of one repeat under ONE variant (variant-major). main() no longer
    uses this -- it interleaves variants per task -- but it is kept for callers."""
    results: list[dict[str, Any]] = []
    batch_runs: list[dict[str, Any]] = []
    for task_group in chunk_list(tasks, effective_shared_guide_batch_size(args, destination)):
        group_results, batch_meta = run_aether_task_group(
            destination=destination,
            doc_name=doc_name,
            doc_text=doc_text,
            task_group=task_group,
            repeat_index=repeat_index,
            args=args,
            doc_token_reference=doc_token_reference,
            options=options,
            on_case_complete=on_case_complete,
        )
        results.extend(group_results)
        if batch_meta is not None:
            batch_runs.append(batch_meta)
    return results, batch_runs


def interleaved_variant_order(variant_count: int, unit_index: int) -> list[int]:
    """The variant order for one task (or batch) unit: a rotation that shifts by
    one each unit, so over a suite every variant runs first equally often.

    Running variants back to back per task, rather than variant-major, keeps a
    slow drift in the serving stack (load, thermal state, a mid-run restart) from
    landing on one variant; rotating the order keeps "always runs second" from
    becoming a variable of its own."""
    if variant_count <= 0:
        return []
    start = unit_index % variant_count
    return [(start + offset) % variant_count for offset in range(variant_count)]


def run_baseline_case(
    *,
    runner: str,
    destination: Destination,
    task: Task,
    repeat_index: int,
    args: argparse.Namespace,
    doc_token_reference: dict[str, Any],
    options: RequestOptions | None = None,
) -> dict[str, Any]:
    """One Python or Rust comparison case (no guide in the prompt)."""
    prompt_builder = build_python_prompt if runner == "python" else build_rust_prompt
    repair_builder = build_python_repair_prompt if runner == "python" else build_rust_repair_prompt
    try:
        case = execute_case(
            initial_prompt=prompt_builder(task),
            destination=destination,
            task=task,
            args=args,
            runner=runner,
            repair_prompt_builder=lambda **kwargs: repair_builder(**kwargs),
            options=options,
        )
    except Exception as exc:  # pragma: no cover - surfaced in JSON report
        case = {
            "attempts": [],
            "generated_ok": False,
            "generation_error": str(exc),
            "run": {
                "returncode": -1,
                "stdout": "",
                "stderr": str(exc),
                "elapsed_seconds": 0.0,
                "exact_stdout_match": False,
            },
            "attempt_count": 0,
            "resolved_after_repair": False,
        }
        case["failure_fingerprint"] = derive_failure_fingerprint(case, task.task_id)
    case["task_id"] = task.task_id
    case["task_title"] = task.title
    case["repeat_index"] = repeat_index
    case["seed"] = None if options is None else options.seed
    case["doc_token_reference"] = doc_token_reference
    return case


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS, help="task manifest JSON")
    parser.add_argument(
        "--docs",
        default=None,
        help="comma-separated doc variants to benchmark: full, medium, small, none, or any --doc NAME "
        "(default: the --doc names when any are given, else full,small)",
    )
    parser.add_argument(
        "--doc",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="define or replace guide variant NAME with the file at PATH; repeatable. For a board, "
        "point it at the docs/ of the same checkout that built --aether-bin",
    )
    parser.add_argument(
        "--aether-root",
        type=pathlib.Path,
        default=DEFAULT_AETHER_ROOT,
        help="aether checkout the default guides come from and the binary's +sha is checked "
        "against (default: components/aether)",
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
        "--start-repeat",
        type=int,
        default=0,
        help="index of the first repeat (default 0); repeat r is requested with seed base + r, so "
        "--start-repeat 2 --repeats 1 re-runs exactly repeat 2",
    )
    parser.add_argument(
        "--seed-base",
        type=int,
        default=None,
        help="base seed for self-hosted destinations without their own `seed` (D37b uses 42); "
        "repeat r sends seed base + r",
    )
    parser.add_argument(
        "--repair-attempts",
        type=int,
        default=0,
        help="when > 0, retry failed cases by feeding diagnostics back to the model",
    )
    parser.add_argument(
        "--repair-feedback-limit",
        type=int,
        default=REPAIR_FEEDBACK_LIMIT_DEFAULT,
        help="max characters of stdout and of stderr included in a repair prompt (stderr after "
        "identical-line runs are collapsed)",
    )
    parser.add_argument(
        "--repair-source-limit",
        type=int,
        default=REPAIR_SOURCE_LIMIT_DEFAULT,
        help="max characters of the previous source in a repair prompt; above it the prompt shows "
        "the head, +/-20 lines round the cited line and the tail (default 8000)",
    )
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN, help="path to local aether binary")
    parser.add_argument(
        "--aether-bin-sha256",
        default="",
        metavar="HEX",
        help="expected sha256 of --aether-bin; a mismatch always aborts. Required, with --allow-skew, "
        "for a standalone build, whose version carries no +sha",
    )
    parser.add_argument(
        "--allow-skew",
        action="store_true",
        help="run although the binary cannot be tied to --aether-root's HEAD (dirty build, '+' "
        "gitlink, +sha mismatch, standalone build); the reasons are recorded in the report",
    )
    parser.add_argument(
        "--aether-arg",
        action="append",
        default=[],
        dest="aether_args",
        metavar="ARG",
        help="extra aether flag placed before the program path on every aether call; repeatable and "
        "recorded. Write flag values with '=': --aether-arg=--strict",
    )
    parser.add_argument(
        "--build-type",
        default=None,
        help="build type to record for the binary (default: read CMAKE_BUILD_TYPE from a "
        "CMakeCache.txt beside it, else unknown)",
    )
    parser.add_argument(
        "--resummarize",
        type=pathlib.Path,
        default=None,
        metavar="REPORT",
        help="recompute and print an existing report's summaries (FA/FX, failure classes, Wilson "
        "CIs) and exit; 3 while infra-failed cases remain. With --output-json, write it back there",
    )
    parser.add_argument(
        "--context-margin",
        type=int,
        default=CONTEXT_MARGIN_TOKENS,
        help="tokens kept free beside measured prompt + output budget (default 512)",
    )
    parser.add_argument(
        "--min-output-tokens",
        type=int,
        default=MIN_OUTPUT_TOKENS,
        help="a request whose clamped output budget falls below this is not sent (context_overflow)",
    )
    parser.add_argument(
        "--allow-unknown-context",
        action="store_true",
        help="run a self-hosted destination whose context window is neither configured "
        "(prompt_context_limit) nor detectable; the context guard is then inert for it",
    )
    parser.add_argument(
        "--allow-broken-oracle",
        action="store_true",
        help="score tasks even when their reference_solution fails on this binary, or the sandbox "
        "probe is not rejected (the failures are recorded under oracle)",
    )
    parser.add_argument(
        "--preflight-only",
        action="store_true",
        help="run the start-up checks (docs, binary snapshot and hash, skew guard) and exit",
    )
    parser.add_argument(
        "--sandbox-deny",
        default="net,proc",
        help="VM 2.0 Phase 6 --deny classes applied to every generated program run (compile_and_run); "
        "this harness runs model-generated code unattended and every task in Tests/aether_doc_bench "
        "is a pure-compute/deterministic-stdout task (no task legitimately needs network or process "
        "spawning), so denying net,proc by default costs nothing and closes a real unattended-execution "
        "risk. Pass an empty string to disable.",
    )
    parser.add_argument(
        "--python-baseline",
        action="store_true",
        help="also ask for Python 3 for each case, run it locally, and record token usage for comparison",
    )
    parser.add_argument(
        "--rust-baseline",
        action="store_true",
        help="also ask for Rust for each case, compile+run it locally with rustc, and record token usage for comparison",
    )
    parser.add_argument(
        "--skip-aether",
        action="store_true",
        help="skip the Aether scoring pass entirely (useful with --python-baseline/--rust-baseline when only a "
        "language-comparison re-run is needed and the destination's Aether score is already known/unchanged)",
    )
    parser.add_argument(
        "--shared-guide-batch-size",
        type=int,
        default=1,
        help="when > 1, batch that many Aether tasks behind one shared guide prompt before splitting results back per case",
    )
    parser.add_argument("--output-json", type=pathlib.Path, default=None, help="write full JSON report to this path")
    parser.add_argument(
        "--keep-loaded",
        action="store_true",
        help="do not ask the T'Ra queue to unload each destination's model when this run is done "
        "with it (a driver that runs the same model again next can pass this to skip a reload)",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="continue the run checkpointed in --output-json: keep its finished cases, run only the "
        "missing ones and those that failed in the infrastructure. Refused when the tasks, binary, "
        "guides, run settings or a destination's model differ. Starts fresh when the file is absent",
    )
    parser.add_argument("--text-summary", action="store_true", help="print compact text summary")
    parser.add_argument("--progress", action="store_true", help="print per-task progress to stderr")
    return parser


def capture_aether_version(aether_bin: pathlib.Path) -> tuple[str, str]:
    """Run ``aether --version`` once; return (version_token, raw_first_line).

    The token is the date-stamp the build carries (YYYY-MM-DD-N, see
    components/aether/VERSION); the raw line is the full first line of output.
    Best-effort: returns ("unknown", "") if the binary cannot be queried, so a
    missing or old binary never aborts a run -- it just records "unknown".
    """
    try:
        proc = subprocess.run(
            [str(aether_bin), "--version"],
            text=True,
            errors="replace",
            capture_output=True,
            timeout=30,
        )
    except Exception:
        return "unknown", ""
    lines = (proc.stdout or proc.stderr or "").strip().splitlines()
    raw = lines[0].strip() if lines else ""
    match = re.search(r"Version:\s*(\S+)", raw)
    return (match.group(1) if match else (raw or "unknown")), raw


def parse_aether_version(token: str) -> dict[str, Any]:
    """Split a version token into its parts.

    Umbrella builds append the component's short commit, with -dirty for a tree
    with uncommitted changes to tracked files (cmake/PscalBuildCommit.cmake):
    `2026-09-22-1+da6028e`, `2026-09-22-1+da6028e-dirty`. A standalone aether
    build reports the VERSION alone (`2026-10-06-1`), so nothing ties it to a
    source commit."""
    token = (token or "").strip()
    version, commit, dirty = token, None, False
    if "+" in token:
        version, _, rest = token.partition("+")
        if rest.endswith("-dirty"):
            dirty, rest = True, rest[: -len("-dirty")]
        commit = rest or None
    elif token.endswith("-dirty"):
        version, dirty = token[: -len("-dirty")], True
    return {"token": token, "version": version, "commit": commit, "dirty": dirty}


def version_tuple(version: str | None) -> tuple[int, ...] | None:
    """`2026-07-26-1` (any +sha suffix ignored) -> (2026, 7, 26, 1), else None."""
    if not version:
        return None
    match = re.match(r"^\s*(\d{4})-(\d{2})-(\d{2})-(\d+)", str(version))
    if not match:
        return None
    return tuple(int(part) for part in match.groups())


def _git(cwd: Any, *argv: str, timeout: int = 20) -> str | None:
    """A read-only git query. GIT_OPTIONAL_LOCKS=0 keeps status/describe from
    refreshing (and so writing) the index of a checkout we only inspect."""
    env = dict(os.environ)
    env["GIT_OPTIONAL_LOCKS"] = "0"
    try:
        proc = subprocess.run(
            ["git", "-C", str(cwd), *argv],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            env=env,
        )
    except Exception:
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.strip()


def git_checkout_info(path: Any, require_toplevel: bool = False) -> dict[str, Any] | None:
    """HEAD, describe and dirty flag of the git checkout holding `path`, or None.

    With require_toplevel, `path` must itself be the checkout's top: an
    uninitialised submodule directory sits inside the umbrella's work tree and
    would otherwise report the umbrella's HEAD as its own."""
    target = pathlib.Path(path)
    directory = target if target.is_dir() else target.parent
    if not directory.exists():
        return None
    top = _git(directory, "rev-parse", "--show-toplevel")
    if not top:
        return None
    if require_toplevel:
        try:
            if pathlib.Path(top).resolve() != directory.resolve():
                return None
        except OSError:
            return None
    status = _git(top, "status", "--porcelain", "--untracked-files=no")
    return {
        "toplevel": display_path(top),
        "head": _git(top, "rev-parse", "HEAD"),
        "describe": _git(top, "describe", "--always", "--dirty"),
        "dirty": bool(status),
    }


def umbrella_submodule_state(name: str = "components/aether") -> dict[str, Any]:
    """`git submodule status` and the recorded gitlink for one umbrella submodule.
    state: ' ' in sync, '+' checkout differs from the gitlink, '-' not
    initialised, 'U' merge conflict."""
    line = _git(REPO_ROOT, "submodule", "status", "--", name) or ""
    tree = _git(REPO_ROOT, "ls-tree", "HEAD", "--", name) or ""
    gitlink = None
    parts = tree.split()
    if len(parts) >= 3 and parts[1] == "commit":
        gitlink = parts[2]
    return {
        "status_line": line or None,
        "state": (line[:1] if line else None),
        "gitlink": gitlink,
    }


def detect_build_type(aether_bin: pathlib.Path) -> tuple[str | None, str | None]:
    """CMAKE_BUILD_TYPE from a CMakeCache.txt beside the binary or one level up."""
    try:
        resolved = aether_bin.resolve()
    except OSError:
        return None, None
    for directory in (resolved.parent, resolved.parent.parent):
        cache = directory / "CMakeCache.txt"
        if not cache.is_file():
            continue
        try:
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("CMAKE_BUILD_TYPE:"):
                    value = line.split("=", 1)[1].strip() if "=" in line else ""
                    return (value or "(none)"), display_path(cache)
        except OSError:
            return None, None
        return "(none)", display_path(cache)
    return None, None


def snapshot_aether_binary(aether_bin: pathlib.Path, run_dir: pathlib.Path) -> dict[str, Any]:
    """Copy the binary into this run's private temp dir and hash the copy.

    Every compile_and_run then executes the copy, so a rebuild of the original
    mid-run (which used to mix two compilers into one report) cannot reach it."""
    source = aether_bin.resolve()
    snapshot = run_dir / source.name
    shutil.copy2(source, snapshot)
    snapshot.chmod(snapshot.stat().st_mode | 0o111)
    digest = sha256_file(snapshot)
    if sha256_file(source) != digest:
        raise SystemExit(f"{aether_bin} changed while it was being copied; re-run when the build is done")
    token, raw = capture_aether_version(snapshot)
    parsed = parse_aether_version(token)
    return {
        "path": snapshot,
        "aether_bin": display_path(aether_bin),
        "aether_bin_resolved": display_path(source),
        "binary_sha256": digest,
        "aether_version": token,
        "aether_version_raw": raw,
        "language_version": parsed["version"],
        "binary_commit": parsed["commit"],
        "binary_dirty": parsed["dirty"],
        "standalone_build": parsed["commit"] is None,
    }


def assess_skew(
    toolchain: dict[str, Any],
    aether_root: pathlib.Path,
    allow_skew: bool,
    expected_sha256: str = "",
) -> dict[str, Any]:
    """The skew guard. Refuses (SystemExit) a binary that cannot be tied to the
    aether checkout the run takes its guides from, unless --allow-skew.

    Refused without --allow-skew:
      * a -dirty binary (built from uncommitted source);
      * a '+' in `git submodule status components/aether` (the checkout is not
        the commit the umbrella records), when --aether-root is that submodule;
      * a +sha that differs from --aether-root's HEAD (or from the recorded
        gitlink, when the submodule is not checked out);
      * a +sha that cannot be checked because --aether-root is not a checkout;
      * a standalone build, which carries no +sha at all. It also needs
        --aether-bin-sha256, so the acceptance names exactly one binary.
    A --aether-bin-sha256 that does not match the binary always aborts."""
    reasons: list[str] = []
    expected = (expected_sha256 or "").strip().lower()
    if expected and expected != toolchain["binary_sha256"]:
        raise SystemExit(
            f"--aether-bin-sha256 {expected} does not match {toolchain['aether_bin']} "
            f"(sha256 {toolchain['binary_sha256']})"
        )

    try:
        is_default_root = aether_root.resolve() == DEFAULT_AETHER_ROOT.resolve()
    except OSError:
        is_default_root = False
    submodule = umbrella_submodule_state() if is_default_root else None
    root_info = git_checkout_info(aether_root, require_toplevel=True)
    commit = toolchain.get("binary_commit")

    if toolchain.get("binary_dirty"):
        reasons.append("the binary was built from a dirty tree (its version carries -dirty)")
    if submodule and submodule.get("state") == "+":
        reasons.append(
            "components/aether is checked out at a commit other than the umbrella's recorded "
            "gitlink ('+' in git submodule status)"
        )
    if submodule and submodule.get("state") == "U":
        reasons.append("components/aether has a merge conflict ('U' in git submodule status)")
    if commit:
        target = root_info.get("head") if root_info else None
        target_label = f"{display_path(aether_root)} HEAD"
        if target is None and submodule and submodule.get("gitlink"):
            target, target_label = submodule["gitlink"], "the umbrella's components/aether gitlink"
        if target is None:
            reasons.append(
                f"cannot check the binary's +{commit}: {display_path(aether_root)} is not a git checkout"
            )
        elif not target.startswith(commit):
            reasons.append(f"the binary is +{commit} but {target_label} is {target[:12]}")
    else:
        reasons.append(
            "standalone build: the binary's version carries no +sha, so nothing ties it to a "
            "source commit"
        )

    if reasons and not allow_skew:
        raise SystemExit(
            "skew guard: refusing to run on this binary:\n  - "
            + "\n  - ".join(reasons)
            + "\nBuild from a clean checkout of the guides' commit, or pass --allow-skew"
            + (" and --aether-bin-sha256 <sha256>" if not commit else "")
            + " (recorded in the report)."
        )
    if reasons and not commit and not expected:
        raise SystemExit(
            "skew guard: a standalone binary needs --allow-skew AND --aether-bin-sha256 "
            f"{toolchain['binary_sha256']} so the run names exactly the binary it accepted"
        )
    return {
        "ok": not reasons,
        "allowed_by_flag": bool(reasons) and allow_skew,
        "reasons": reasons,
        "expected_binary_sha256": expected or None,
        "aether_root": display_path(aether_root),
        "aether_root_checkout": root_info,
        "umbrella_submodule": submodule,
    }


def prompt_template_fingerprint() -> dict[str, Any]:
    """sha256 of every prompt template, rendered with placeholder inputs.

    The guide, task and repair inputs each have their own hash; this names the
    fixed wording around them. D41: it (with the harness sha) replaces the
    harness-only guide-stamp bumps, so any wording change shows up here."""
    task = Task(
        task_id="{TASK_ID}",
        title="{TASK_TITLE}",
        prompt="{TASK_PROMPT}",
        expected_stdout="{EXPECTED_STDOUT}",
    )
    hidden = Task(task_id="{TASK_ID}", title="{TASK_TITLE}", prompt="{TASK_PROMPT}",
                  expected_stdout="{EXPECTED_STDOUT}", hide_expected_stdout=True)
    status = Task(task_id="{TASK_ID}", title="{TASK_TITLE}", prompt="{TASK_PROMPT}",
                  expected_stdout="{EXPECTED_STDOUT}", expected_returncode=3)
    repair_inputs = dict(
        previous_source="{PREVIOUS_SOURCE}",
        attempt_number=1,
        failure_summary="{FAILURE_SUMMARY}",
        observed_stdout="{OBSERVED_STDOUT}",
        observed_stderr="{OBSERVED_STDERR}",
    )
    rendered = {
        "initial": build_prompt("{DOC_NAME}", "{DOC_TEXT}", task),
        "initial_none": build_prompt("none", "", task),
        "batch": build_batch_prompt("{DOC_NAME}", "{DOC_TEXT}", [task]),
        "repair": build_repair_prompt(doc_name="{DOC_NAME}", doc_text="{DOC_TEXT}", task=task, **repair_inputs),
        "python": build_python_prompt(task),
        "python_repair": build_python_repair_prompt(task=task, **repair_inputs),
        "rust": build_rust_prompt(task),
        "rust_repair": build_rust_repair_prompt(task=task, **repair_inputs),
        "initial_hidden": build_prompt("{DOC_NAME}", "{DOC_TEXT}", hidden),
        "initial_exit_status": build_prompt("{DOC_NAME}", "{DOC_TEXT}", status),
        "repair_hidden": build_repair_prompt(doc_name="{DOC_NAME}", doc_text="{DOC_TEXT}", task=hidden, **repair_inputs),
        "repair_exit_status": build_repair_prompt(doc_name="{DOC_NAME}", doc_text="{DOC_TEXT}", task=status, **repair_inputs),
        "batch_hidden": build_batch_prompt("{DOC_NAME}", "{DOC_TEXT}", [hidden]),
    }
    per_template = {name: sha256_text(text) for name, text in sorted(rendered.items())}
    return {
        "sha256": sha256_text(json.dumps(per_template, sort_keys=True)),
        "templates": per_template,
    }


HARNESS_SOURCE_FILES = ("tools/aether_doc_bench.py", "tools/fleet_env.py", "tools/aether_oracle_check.py")


def harness_fingerprint() -> dict[str, Any]:
    """Identity of the harness code: the umbrella commit, whether the harness
    sources differ from it, and a content hash of those sources (which holds even
    outside git)."""
    files: dict[str, str] = {}
    for rel in HARNESS_SOURCE_FILES:
        path = REPO_ROOT / rel
        if path.is_file():
            files[rel] = sha256_file(path)
    dirty_files = _git(REPO_ROOT, "status", "--porcelain", "--untracked-files=no", "--", *HARNESS_SOURCE_FILES)
    return {
        "sha256": sha256_text(json.dumps(files, sort_keys=True)),
        "files": files,
        "umbrella_head": _git(REPO_ROOT, "rev-parse", "HEAD"),
        "harness_dirty": bool(dirty_files),
    }


def guide_record(name: str, path: pathlib.Path | None, text: str) -> dict[str, Any]:
    """What a report says about one guide variant: path, stamp, size and hash,
    plus the git state of the checkout the file lives in."""
    if path is None:
        return {
            "path": None,
            "version": None,
            "sha256": None,
            "bytes": 0,
            "approx_tokens": 0,
            "tokens_o200k": 0,
            "checkout": None,
        }
    return {
        "path": display_path(path),
        "version": guide_version(text),
        "sha256": sha256_text(text),
        "bytes": len(text.encode("utf-8")),
        "approx_tokens": approx_tokens(text),
        "tokens_o200k": tokens_o200k(text),
        "checkout": git_checkout_info(path),
    }


def _exit_through_finally(signum, _frame) -> None:
    # SIGTERM/SIGHUP (a driver killed, a screen closed) would otherwise end the
    # process without running `finally`, leaving the run's model loaded.
    raise SystemExit(128 + signum)


def main(argv: list[str] | None = None) -> int:
    import signal

    for sig in (signal.SIGTERM, signal.SIGHUP):
        try:
            signal.signal(sig, _exit_through_finally)
        except (ValueError, OSError):  # not the main thread, or no such signal here
            pass
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.resummarize is not None:
        if args.output_json and args.output_json.resolve() != args.resummarize.resolve():
            raise SystemExit("--resummarize writes back only to the same file (--output-json REPORT)")
        return resummarize_report(args.resummarize, write=args.output_json is not None)

    tasks = load_tasks(args.tasks)
    tasks_bytes = args.tasks.read_bytes()

    # Record the dataset's own version stamp (YYYY-MM-DD-N), parallel to the
    # language version, so a score ties to the exact dataset revision too.
    try:
        _traw = json.loads(tasks_bytes.decode("utf-8"))
        tasks_version = _traw.get("version") if isinstance(_traw, dict) else None
    except Exception:
        tasks_version = None

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

    destinations_sha256 = None
    if args.destinations_config.exists():
        destinations_sha256 = sha256_file(args.destinations_config)
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
    if args.repeats < 1:
        raise SystemExit("--repeats must be at least 1")
    if args.start_repeat < 0:
        raise SystemExit("--start-repeat must be >= 0")
    if args.resume and not args.output_json:
        raise SystemExit("--resume needs --output-json (the checkpointed report to continue)")

    doc_overrides = parse_doc_overrides(args.doc)
    variants = doc_variant_paths(args.aether_root, doc_overrides)
    if args.docs is not None:
        doc_names = [part.strip() for part in args.docs.split(",") if part.strip()]
    elif doc_overrides:
        doc_names = list(doc_overrides)
    else:
        doc_names = ["full", "small"]
    doc_variants = resolve_docs(doc_names, variants)
    doc_texts = {name: (read_text(path) if path else "") for name, path in doc_variants}

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    run_dir = pathlib.Path(tempfile.mkdtemp(prefix="aether-bench-run-"))
    try:
        return _run_benchmark(
            args=args,
            run_dir=run_dir,
            tasks=tasks,
            tasks_bytes=tasks_bytes,
            tasks_version=tasks_version,
            destinations=destinations,
            destinations_sha256=destinations_sha256,
            doc_variants=doc_variants,
            doc_texts=doc_texts,
            variants=variants,
        )
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)


def _run_benchmark(
    *,
    args: argparse.Namespace,
    run_dir: pathlib.Path,
    tasks: list[Task],
    tasks_bytes: bytes,
    tasks_version: Any,
    destinations: list[Destination],
    destinations_sha256: str | None,
    doc_variants: list[tuple[str, pathlib.Path | None]],
    doc_texts: dict[str, str],
    variants: dict[str, pathlib.Path | None],
) -> int:
    # Snapshot and hash the binary once, check it against the guides' checkout,
    # then point every aether call at the snapshot.
    original_bin = args.aether_bin
    toolchain = snapshot_aether_binary(original_bin, run_dir)
    skew = assess_skew(toolchain, args.aether_root, args.allow_skew, args.aether_bin_sha256)
    if args.build_type:
        build_type, build_type_source = args.build_type, "--build-type"
    else:
        build_type, build_type_source = detect_build_type(original_bin)
    toolchain.update({
        "build_type": build_type or "unknown",
        "build_type_source": build_type_source,
        "aether_args": list(args.aether_args or []),
        "sandbox_deny": args.sandbox_deny,
        "snapshot": True,
    })
    snapshot_path = toolchain.pop("path")
    args.aether_bin = snapshot_path
    args.aether_bin_display = toolchain["aether_bin"]
    args.binary_sha256 = toolchain["binary_sha256"]

    harness = harness_fingerprint()
    harness["prompt_template"] = prompt_template_fingerprint()
    umbrella_status = _git(REPO_ROOT, "status", "--porcelain", "--untracked-files=no")
    provenance = {
        "umbrella_head": harness["umbrella_head"],
        "umbrella_dirty": bool(umbrella_status),
        "aether_root": display_path(args.aether_root),
        "aether_root_checkout": skew["aether_root_checkout"],
        "components_aether_submodule": skew["umbrella_submodule"] or umbrella_submodule_state(),
    }

    guides: dict[str, dict[str, Any]] = {}
    for name, path in variants.items():
        if path is not None and not pathlib.Path(path).is_file():
            continue
        text = doc_texts.get(name)
        if text is None:
            text = read_text(path) if path else ""
        guides[name] = guide_record(name, path, text)
    # Each case carries this slim copy (the full record, with each guide's
    # checkout, is the report-level "guides" block).
    doc_token_reference: dict[str, dict[str, Any]] = {
        name: {key: record[key] for key in ("path", "version", "sha256", "bytes", "approx_tokens", "tokens_o200k")}
        for name, record in guides.items()
    }

    report: dict[str, Any] = {
        "tasks_file": display_path(args.tasks),
        "tasks_version": tasks_version,
        "tasks_sha256": sha256_bytes(tasks_bytes),
        "destinations_config": display_path(args.destinations_config),
        "destinations_sha256": destinations_sha256,
        "created_at_unix": int(time.time()),
        "aether_version": toolchain["aether_version"],
        "aether_version_raw": toolchain["aether_version_raw"],
        "aether_bin": toolchain["aether_bin"],
        "binary_sha256": toolchain["binary_sha256"],
        "toolchain": toolchain,
        "skew_guard": {key: skew[key] for key in ("ok", "allowed_by_flag", "reasons", "expected_binary_sha256")},
        "provenance": provenance,
        "harness": harness,
        "run_config": {
            "docs": [name for name, _ in doc_variants],
            "repeats": args.repeats,
            "start_repeat": args.start_repeat,
            "seed_base": args.seed_base,
            "repair_attempts": args.repair_attempts,
            "repair_feedback_limit": args.repair_feedback_limit,
            "repair_source_limit": args.repair_source_limit,
            "context_margin": args.context_margin,
            "min_output_tokens": args.min_output_tokens,
            "allow_unknown_context": bool(args.allow_unknown_context),
            "shared_guide_batch_size": args.shared_guide_batch_size,
            "python_baseline": bool(args.python_baseline),
            "rust_baseline": bool(args.rust_baseline),
            "skip_aether": bool(args.skip_aether),
            "variant_order": "interleaved per task, rotated",
            "task_ids": [task.task_id for task in tasks],
        },
        "summary": {
            "total_cases_per_destination": len(tasks) * args.repeats,
            "doc_variants": len(doc_variants),
            "destination_count": len(destinations),
        },
        "guides": guides,
        "doc_token_reference": doc_token_reference,
        "destinations": [],
    }

    # Oracle pre-flight (W1-10): every selected task's reference must still
    # print its expected stdout on THIS binary with THESE flags, and the
    # sandbox must still refuse a socket -- before any model is paid for.
    import aether_oracle_check as oracle_check

    oracle_refs = oracle_check.check_references(tasks, args)
    sandbox_probe = oracle_check.check_sandbox(args) if "net" in (args.sandbox_deny or "") else None
    broken = sorted(tid for tid, result in oracle_refs.items() if result["ok"] is False)
    report["oracle"] = {
        "references": oracle_refs,
        "broken": broken,
        "sandbox_probe": sandbox_probe,
        "allowed_broken": bool(args.allow_broken_oracle),
    }
    oracle_status = {tid: result["ok"] for tid, result in oracle_refs.items()}
    oracle_problems = [f"reference {tid}: {oracle_refs[tid]['detail']}" for tid in broken]
    if sandbox_probe and not sandbox_probe["ok"]:
        oracle_problems.append(f"sandbox probe was not rejected under --deny {sandbox_probe['deny']}")
    if oracle_problems and not args.allow_broken_oracle:
        raise SystemExit(
            "oracle pre-flight failed on this binary:\n  - " + "\n  - ".join(oracle_problems)
            + "\nFix the task (or the compiler), or pass --allow-broken-oracle (recorded)."
        )

    context_limits: dict[str, dict[str, Any]] = {}
    unknown_context: list[str] = []
    for destination in destinations:
        limit, source = resolve_context_limit(destination)
        context_limits[destination.destination_id] = {"context_limit": limit, "context_source": source}
        if limit is None and context_limit_required(destination):
            unknown_context.append(destination.destination_id)
    if unknown_context and not args.allow_unknown_context:
        raise SystemExit(
            "context guard: cannot determine the context window of "
            + ", ".join(unknown_context)
            + "; set prompt_context_limit on the destination (or pass --allow-unknown-context, "
            "which leaves the guard inert for it)"
        )
    report["context_limits"] = context_limits

    if args.preflight_only:
        print(json.dumps({
            "oracle": {"checked": sum(1 for v in oracle_status.values() if v is not None),
                       "broken": broken,
                       "sandbox_probe_ok": None if sandbox_probe is None else sandbox_probe["ok"]},
            "context_limits": context_limits,
            "preflight": "ok",
            "aether_version": toolchain["aether_version"],
            "binary_sha256": toolchain["binary_sha256"],
            "skew_guard": report["skew_guard"],
            "docs": {name: guides.get(name, {}).get("sha256") for name, _ in doc_variants},
            "tasks": len(tasks),
            "tasks_sha256": report["tasks_sha256"],
        }, indent=2))
        return 0

    # --resume: a run killed part-way (a host crash, a laptop asleep, a wedged
    # backend) continues from the report it checkpointed after every case,
    # instead of paying for the finished cases again.
    resume_kept: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    prior_destination_reports: dict[str, dict[str, Any]] = {}
    if args.resume and args.output_json.exists():
        prior = json.loads(args.output_json.read_text(encoding="utf-8"))
        problems = resume_mismatches(prior, report, {d.destination_id: d.model for d in destinations})
        if problems:
            raise SystemExit(
                f"--resume: {args.output_json} was not made by this configuration:\n  - "
                + "\n  - ".join(problems)
                + "\nRun it to a new --output-json, or restore the configuration it was made with."
            )
        resume_kept, n_kept, n_dropped = resume_kept_cases(prior)
        prior_destination_reports = {d["destination_id"]: d for d in prior.get("destinations") or []}
        report["created_at_unix"] = prior.get("created_at_unix", report["created_at_unix"])
        report["resumed"] = list(prior.get("resumed") or []) + [{
            "at_unix": int(time.time()),
            "kept_cases": n_kept,
            "dropped_infra_cases": n_dropped,
            "prior_updated_at_unix": prior.get("updated_at_unix"),
            "prior_destinations_sha256": prior.get("destinations_sha256"),
            "prior_harness_sha256": ((prior.get("harness") or {}).get("files") or {}).get("tools/aether_doc_bench.py"),
        }]
        print(f"[resume] {args.output_json}: keeping {n_kept} finished case(s), re-running "
              f"{n_dropped} infra-failed case(s) and every missing one", file=sys.stderr)
    elif args.resume:
        print(f"[resume] {args.output_json} does not exist yet; starting a fresh run", file=sys.stderr)

    report_lock = threading.Lock()

    def persist_report_checkpoint() -> None:
        if not args.output_json:
            return
        report["updated_at_unix"] = int(time.time())
        write_json_atomic(args.output_json, report)

    def refresh_variant_report(variant_report: dict[str, Any]) -> None:
        refresh_variant_summaries(variant_report)

    for destination in destinations:
        ok, detail = preflight_destination(destination)
        if not ok:
            print(
                f"[preflight] SKIPPING {destination.destination_id} ({destination.model}): {detail}",
                file=sys.stderr,
            )
            report.setdefault("preflight_failures", []).append({
                "destination_id": destination.destination_id,
                "model": destination.model,
                "detail": detail,
            })
            if destination.destination_id in prior_destination_reports:
                # Never lose checkpointed cases because a host is down today.
                report["destinations"].append(prior_destination_reports[destination.destination_id])
            continue

        destination_report = {
            "destination_id": destination.destination_id,
            "type": destination.kind,
            "model": destination.model,
            "base_url": destination.base_url,
            # The seed repeat 0 is requested with; repeat r adds r.
            "seed_base": request_seed(destination, 0, args.seed_base),
            "context_limit": context_limits[destination.destination_id]["context_limit"],
            "context_source": context_limits[destination.destination_id]["context_source"],
            "max_output_tokens": destination.max_output_tokens,
            "variants": [],
        }
        report["destinations"].append(destination_report)

        variant_reports: list[dict[str, Any]] = []
        for doc_name, doc_path in doc_variants:
            doc_text = doc_texts[doc_name]
            record = guides.get(doc_name) or guide_record(doc_name, doc_path, doc_text)
            batch_size = effective_shared_guide_batch_size(args, destination)
            variant_report = {
                "doc_name": doc_name,
                "doc_path": record["path"],
                "doc_version": guide_version(doc_text),
                "doc_sha256": record["sha256"],
                "doc_bytes": len(doc_text.encode("utf-8")),
                "doc_approx_tokens": approx_tokens(doc_text) if doc_text else 0,
                "doc_tokens_o200k": record.get("tokens_o200k"),
                "shared_guide_batch_size_requested": max(1, int(args.shared_guide_batch_size)),
                "shared_guide_batch_size": batch_size,
                "batch_mode_enabled": bool(batch_size > 1),
                "batch_runs": [],
                "results": [],
            }
            if args.python_baseline:
                variant_report["python_baseline_results"] = []
            if args.rust_baseline:
                variant_report["rust_baseline_results"] = []
            for key in CASE_KEYS:
                if key in variant_report:
                    variant_report[key].extend(resume_kept.get((destination.destination_id, doc_name, key), []))
            prior_dest = prior_destination_reports.get(destination.destination_id) or {}
            for prior_variant in prior_dest.get("variants") or []:
                if prior_variant.get("doc_name") == doc_name:
                    variant_report["batch_runs"].extend(prior_variant.get("batch_runs") or [])
            refresh_variant_report(variant_report)
            destination_report["variants"].append(variant_report)
            variant_reports.append(variant_report)
        persist_report_checkpoint()

        sequence = {"next": 1 + max(
            (case.get("case_sequence", -1) for vr in variant_reports for key in CASE_KEYS
             for case in vr.get(key, [])), default=-1)}
        # (variant index, case key, repeat) -> task ids already measured, from --resume.
        done: dict[tuple[int, str, int], set[str]] = {}
        for vi, vr in enumerate(variant_reports):
            for key in CASE_KEYS:
                for case in vr.get(key, []):
                    done.setdefault((vi, key, case.get("repeat_index")), set()).add(case.get("task_id"))

        def append_case(variant_report: dict[str, Any], key: str, case_record: dict[str, Any]) -> None:
            with report_lock:
                case_record["oracle_ok"] = oracle_status.get(case_record.get("task_id"))
                case_record["case_sequence"] = sequence["next"]
                sequence["next"] += 1
                variant_report[key].append(case_record)
                refresh_variant_report(variant_report)
                persist_report_checkpoint()

        groups = chunk_list(tasks, effective_shared_guide_batch_size(args, destination))
        workers = max(1, int(os.environ.get("AETHER_BENCH_WORKERS", "1") or "1"))
        try:
            for repeat_index in range(args.start_repeat, args.start_repeat + args.repeats):
                options = RequestOptions(seed=request_seed(destination, repeat_index, args.seed_base))
                units: list[tuple[int, int]] = []
                for group_index, _group in enumerate(groups):
                    for variant_index in interleaved_variant_order(len(variant_reports), group_index + repeat_index):
                        units.append((group_index, variant_index))

                def run_unit(unit: tuple[int, int]) -> None:
                    group_index, variant_index = unit
                    group = groups[group_index]
                    variant_report = variant_reports[variant_index]
                    doc_name, _doc_path = doc_variants[variant_index]
                    finished = done.get((variant_index, "results", repeat_index), set())
                    aether_group = [task for task in group if task.task_id not in finished]
                    if not args.skip_aether and aether_group:
                        _results, batch_meta = run_aether_task_group(
                            destination=destination,
                            doc_name=doc_name,
                            doc_text=doc_texts[doc_name],
                            task_group=aether_group,
                            repeat_index=repeat_index,
                            args=args,
                            doc_token_reference=doc_token_reference,
                            options=options,
                            on_case_complete=lambda case_record, vr=variant_report: append_case(vr, "results", case_record),
                        )
                        if batch_meta is not None:
                            with report_lock:
                                variant_report["batch_runs"].append(batch_meta)
                    for runner, key, enabled in (
                        ("python", "python_baseline_results", args.python_baseline),
                        ("rust", "rust_baseline_results", args.rust_baseline),
                    ):
                        if not enabled:
                            continue
                        for task in group:
                            if task.task_id in done.get((variant_index, key, repeat_index), set()):
                                continue
                            case = run_baseline_case(
                                runner=runner,
                                destination=destination,
                                task=task,
                                repeat_index=repeat_index,
                                args=args,
                                doc_token_reference=doc_token_reference,
                                options=options,
                            )
                            append_case(variant_report, key, case)

                if workers > 1 and len(units) > 1:
                    # Concurrent fan-out (LM Studio PARALLEL and the like). Units are
                    # submitted in the interleaved order; results land as they finish
                    # and carry case_sequence, so the order stays reconstructible.
                    import concurrent.futures as _cf

                    with _cf.ThreadPoolExecutor(max_workers=workers) as executor:
                        for future in [executor.submit(run_unit, unit) for unit in units]:
                            future.result()
                else:
                    for unit in units:
                        run_unit(unit)
                with report_lock:
                    for variant_report in variant_reports:
                        refresh_variant_report(variant_report)
                    persist_report_checkpoint()
        finally:
            # Also on an interrupt: a killed run must not leave its model resident.
            if not args.keep_loaded:
                destination_report["model_release"] = release_destination_model(destination)
                with report_lock:
                    persist_report_checkpoint()

    # --resume with fewer destinations than the report: keep the others as they were.
    ran = {d.get("destination_id") for d in report["destinations"]}
    for dest_id, prior_dest in prior_destination_reports.items():
        if dest_id not in ran:
            report["destinations"].append(prior_dest)

    # The snapshot must still be the binary every case was scored with.
    final_sha = sha256_file(snapshot_path)
    report["toolchain"]["binary_sha256_at_end"] = final_sha
    if final_sha != toolchain["binary_sha256"]:
        report["toolchain"]["snapshot_modified"] = True
        print("[toolchain] WARNING: the binary snapshot changed during the run", file=sys.stderr)

    if args.output_json:
        persist_report_checkpoint()

    if args.text_summary or not args.output_json:
        print_text_summary(report)

    infra = report_infra_failed(report)
    report["infra_failed"] = infra
    if args.output_json:
        persist_report_checkpoint()
    if infra:
        # No headline while any case measured nothing (D37d): re-run them.
        print(
            f"[infra] {infra} case(s) failed in the infrastructure and were not measured; no headline. "
            f"Re-run them: python3 Tests/aether_doc_bench/rerun_nogen_cases.py --report "
            f"{args.output_json or '<report.json>'} --apply",
            file=sys.stderr,
        )
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
