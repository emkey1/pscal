#!/usr/bin/env python3
"""Benchmark Aether guide variants against code-generation tasks.

This harness compares one or more Aether guide documents by prompting an LLM
to solve the same manifest-defined programming tasks, then compiling/running
the generated source with the local `aether` binary.

Two model adapters are supported:

1. `openai`: calls the Responses API with a configured model.
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
import subprocess
import sys
import tempfile
import textwrap
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_TASKS = REPO_ROOT / "Tests" / "aether_doc_bench" / "tasks.json"
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_DESTINATIONS_CONFIG = REPO_ROOT / "Tests" / "aether_doc_bench" / "destinations.template.json"
DOC_VARIANTS = {
    "full": REPO_ROOT / "Docs" / "aether_for_llms_and_others.md",
    "small": REPO_ROOT / "Docs" / "aether_for_llms_with_small_contexts.md",
}
_DESTINATION_CONTEXT_CACHE: dict[tuple[str, str, str], int | None] = {}


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


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def approx_tokens(text: str) -> int:
    # Coarse but stable enough for comparing prompt-footprint impact.
    return max(1, (len(text) + 3) // 4)


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
            )
        )
    return destinations


def resolve_docs(names: list[str]) -> list[tuple[str, pathlib.Path]]:
    resolved: list[tuple[str, pathlib.Path]] = []
    for name in names:
        if name not in DOC_VARIANTS:
            raise SystemExit(f"unknown doc variant '{name}', expected one of: {', '.join(DOC_VARIANTS)}")
        resolved.append((name, DOC_VARIANTS[name]))
    return resolved


def sanitize_code(raw: str) -> str:
    text = raw.strip()
    if text.startswith("```"):
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

        Use the following Aether guide as the ground truth for syntax, supported
        features, and style.

        Guide variant: {doc_name}
        --- BEGIN AETHER GUIDE ---
        {doc_text}
        --- END AETHER GUIDE ---

        Write exactly one complete Aether program that solves the task below.

        Requirements:
        - Return only raw Aether source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - Keep the program self-contained unless the task explicitly provides files.
        - The program must compile and run with the local `aether` compiler.
        - The program must print exactly the expected output.

        Task ID: {task.task_id}
        Task Title: {task.title}
        Task:
        {task.prompt}

        Expected stdout:
        {task.expected_stdout}
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

        Use the following Aether guide as the ground truth for syntax, supported
        features, and style.

        Guide variant: {doc_name}
        --- BEGIN AETHER GUIDE ---
        {doc_text}
        --- END AETHER GUIDE ---

        Your previous attempt did not satisfy the benchmark task.
        Return one full corrected Aether program.

        Requirements:
        - Return only raw Aether source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
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
        """
    )


def resolve_api_key(destination: Destination) -> str | None:
    if destination.api_key is not None:
        return destination.api_key
    if destination.api_key_env:
        return os.environ.get(destination.api_key_env)
    return os.environ.get("OPENAI_API_KEY")


def http_json_request(
    url: str,
    body: dict[str, Any],
    api_key: str | None,
    timeout_seconds: int = 120,
) -> dict[str, Any]:
    headers = {
        "Content-Type": "application/json",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    req = urllib.request.Request(
        url,
        method="POST",
        data=json.dumps(body).encode("utf-8"),
        headers=headers,
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout_seconds) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP API error {exc.code}: {detail}") from exc
    except TimeoutError as exc:
        raise RuntimeError(f"HTTP API request timed out after {timeout_seconds} seconds") from exc
    except urllib.error.URLError as exc:
        reason = getattr(exc, "reason", exc)
        raise RuntimeError(f"HTTP API request failed: {reason}") from exc


def http_json_get(url: str, api_key: str | None) -> dict[str, Any]:
    headers: dict[str, str] = {}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    req = urllib.request.Request(url, method="GET", headers=headers)

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))
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
    }
    if destination.temperature >= 0:
        body["temperature"] = destination.temperature

    payload = http_json_request(
        f"{base_url}/chat/completions",
        body,
        api_key,
        timeout_seconds=destination.request_timeout_seconds,
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


def invoke_command(prompt: str, command_template: str, cwd: pathlib.Path) -> dict[str, Any]:
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
            capture_output=True,
            timeout=180,
        )
    finally:
        prompt_file.unlink(missing_ok=True)

    if proc.returncode != 0:
        raise RuntimeError(
            "command provider failed with exit code "
            f"{proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )

    return {
        "raw_text": proc.stdout,
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
    if destination.kind == "command":
        if not destination.command_template:
            raise RuntimeError("command_template is required for command destinations")
        return invoke_command(prompt=prompt, command_template=destination.command_template, cwd=REPO_ROOT)
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
            "elapsed_seconds": round(elapsed, 3),
            "exact_stdout_match": exact_match,
        }


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
) -> dict[str, Any]:
    prompt_tokens = approx_tokens(prompt)
    attempt: dict[str, Any] = {
        "prompt_kind": prompt_kind,
        "prompt_approx_tokens": prompt_tokens,
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
    attempt["generated_ok"] = bool(source_code.strip())
    attempt["source_code"] = source_code
    if attempt["generated_ok"]:
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
            print(
                f"{variant['doc_name']:>5}  "
                f"approx_tokens={variant['doc_approx_tokens']:<6}  "
                f"generated={variant['summary']['generated_ok']}/{variant['summary']['total_cases']}  "
                f"run={variant['summary']['run_ok']}/{variant['summary']['total_cases']}  "
                f"exact={variant['summary']['exact_stdout_match']}/{variant['summary']['total_cases']}  "
                f"repaired={variant['summary']['resolved_after_repair']}/{variant['summary']['total_cases']}"
            )
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


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS, help="task manifest JSON")
    parser.add_argument(
        "--docs",
        default="full,small",
        help="comma-separated doc variants to benchmark (default: full,small)",
    )
    parser.add_argument("--task", action="append", default=[], help="restrict to one or more task ids")
    parser.add_argument("--list-tasks", action="store_true", help="list manifest task ids and exit")
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
        "destinations": [],
    }

    for destination in destinations:
        destination_report = {
            "destination_id": destination.destination_id,
            "type": destination.kind,
            "model": destination.model,
            "base_url": destination.base_url,
            "variants": [],
        }
        for doc_name, doc_path in doc_variants:
            doc_text = read_text(doc_path)
            results: list[dict[str, Any]] = []

            for task in tasks:
                for repeat_index in range(args.repeats):
                    if args.progress:
                        print_progress_start(destination, doc_name, task, repeat_index)
                    prompt = build_prompt(doc_name=doc_name, doc_text=doc_text, task=task)
                    case_record: dict[str, Any] = {
                        "task_id": task.task_id,
                        "task_title": task.title,
                        "repeat_index": repeat_index,
                        "attempts": [],
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
                        results.append(case_record)
                        try:
                            run_destination_cleanup(destination, task, doc_name, repeat_index)
                        except Exception as cleanup_exc:  # pragma: no cover - surfaced in JSON report
                            case_record["cleanup_error"] = str(cleanup_exc)
                        continue
                    try:
                        attempt = evaluate_attempt(
                            prompt=prompt,
                            prompt_kind="initial",
                            destination=destination,
                            task=task,
                            args=args,
                        )
                        case_record["attempts"].append(attempt)

                        for repair_index in range(args.repair_attempts):
                            if attempt["run"]["exact_stdout_match"]:
                                break
                            failure_summary = derive_failure_summary(
                                attempt["generated_ok"],
                                attempt["run"],
                                attempt.get("generation_error"),
                            )
                            repair_prompt = build_repair_prompt(
                                doc_name=doc_name,
                                doc_text=doc_text,
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
                            )
                            case_record["attempts"].append(attempt)

                        final_attempt = case_record["attempts"][-1]
                        case_record.update(final_attempt)
                        case_record["attempt_count"] = len(case_record["attempts"])
                        case_record["resolved_after_repair"] = (
                            case_record["run"]["exact_stdout_match"] and len(case_record["attempts"]) > 1
                        )
                        case_record["failure_fingerprint"] = (
                            "" if case_record["run"]["exact_stdout_match"] else derive_failure_fingerprint(case_record)
                        )
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
                    results.append(case_record)

            variant_report = {
                "doc_name": doc_name,
                "doc_path": str(doc_path),
                "doc_bytes": len(doc_text.encode("utf-8")),
                "doc_approx_tokens": approx_tokens(doc_text),
                "results": results,
                "summary": summarize(results),
                "failure_patterns": summarize_failure_patterns(results),
            }
            destination_report["variants"].append(variant_report)
        report["destinations"].append(destination_report)

    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(report, indent=2), encoding="utf-8")

    if args.text_summary or not args.output_json:
        print_text_summary(report)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
