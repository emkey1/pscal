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


def resolve_api_key(destination: Destination) -> str | None:
    if destination.api_key is not None:
        return destination.api_key
    if destination.api_key_env:
        return os.environ.get(destination.api_key_env)
    return os.environ.get("OPENAI_API_KEY")


def http_json_request(url: str, body: dict[str, Any], api_key: str | None) -> dict[str, Any]:
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
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP API error {exc.code}: {detail}") from exc


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

    payload = http_json_request(f"{base_url}/responses", body, api_key)
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

    payload = http_json_request(f"{base_url}/chat/completions", body, api_key)
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


def summarize(results: list[dict[str, Any]]) -> dict[str, Any]:
    total = len(results)
    generated = sum(1 for item in results if item["generated_ok"])
    compiled_and_ran = sum(1 for item in results if item["run"]["returncode"] == 0)
    exact = sum(1 for item in results if item["run"]["exact_stdout_match"])
    return {
        "total_cases": total,
        "generated_ok": generated,
        "run_ok": compiled_and_ran,
        "exact_stdout_match": exact,
        "generated_rate": round(generated / total, 4) if total else 0.0,
        "run_rate": round(compiled_and_ran / total, 4) if total else 0.0,
        "exact_match_rate": round(exact / total, 4) if total else 0.0,
    }


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
                f"exact={variant['summary']['exact_stdout_match']}/{variant['summary']['total_cases']}"
            )
        print("")


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
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN, help="path to local aether binary")
    parser.add_argument("--output-json", type=pathlib.Path, default=None, help="write full JSON report to this path")
    parser.add_argument("--text-summary", action="store_true", help="print compact text summary")
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
                    prompt = build_prompt(doc_name=doc_name, doc_text=doc_text, task=task)
                    case_record: dict[str, Any] = {
                        "task_id": task.task_id,
                        "task_title": task.title,
                        "repeat_index": repeat_index,
                        "prompt_approx_tokens": approx_tokens(prompt),
                    }
                    try:
                        generation = run_model(prompt, destination)
                        source_code = sanitize_code(generation["raw_text"])
                        case_record["generation"] = generation
                        case_record["generated_ok"] = bool(source_code.strip())
                        case_record["source_code"] = source_code
                        if case_record["generated_ok"]:
                            case_record["run"] = compile_and_run(task, source_code, args)
                        else:
                            case_record["run"] = {
                                "returncode": -1,
                                "stdout": "",
                                "stderr": "empty model output",
                                "elapsed_seconds": 0.0,
                                "exact_stdout_match": False,
                            }
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
                    results.append(case_record)

            variant_report = {
                "doc_name": doc_name,
                "doc_path": str(doc_path),
                "doc_bytes": len(doc_text.encode("utf-8")),
                "doc_approx_tokens": approx_tokens(doc_text),
                "results": results,
                "summary": summarize(results),
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
