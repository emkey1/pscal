#!/usr/bin/env python3
"""Experimental interactive-session variant of aether_doc_bench.py.

The default harness (aether_doc_bench.py) sends the full guide document fresh
in every request -- one task, one isolated prompt, no memory of prior tasks.
That is deliberate: it is what makes every row in
components/aether/docs/aether_guided_benchmark.md an independent measurement
of "guide alone, nothing else." It is also expensive and slow -- the ~16k-token
full guide gets re-transmitted (and re-processed) on every single task.

This script tests an alternative: one growing session per destination per doc
variant. The guide is sent ONCE, as part of the first task's prompt. Every
task after that sends only a short "ignoring previous requests" reset
instruction plus the new task -- no guide text -- relying on the destination
already having it in-session (via true server-side state for the OpenAI
Responses API's `previous_response_id`, or via the provider's own prompt-prefix
caching for everyone else, since the growing transcript is an exact repeated
prefix each turn).

This is explicitly NOT a drop-in replacement. The reset instruction is a soft
steering signal, not a hard context wipe -- a model can still be influenced by
everything earlier in its own transcript even after being told to disregard
it. Whether that measurably changes scores (especially on later-position
tasks, the signature of contamination) is exactly what this script exists to
let you check empirically before deciding whether session mode is usable for
anything beyond exploration. Compare its report against an
aether_doc_bench.py run over the same models/tasks/doc variant.

Only two destination kinds are supported: openai_responses (true stateful
threading via previous_response_id) and openai_chat_completions (growing
messages list, resent in full every turn -- this only saves anything if the
destination's provider does automatic prefix-caching; unlike the Responses
API there is no way to confirm that from the client side).
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import textwrap
from typing import Any

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_doc_bench as adb  # noqa: E402

REPO_ROOT = adb.REPO_ROOT
OUTPUT_END_MARKER = adb.OUTPUT_END_MARKER

RESET_INSTRUCTION = (
    "Ignoring previous requests, but still using the Aether Guide above, write exactly "
    "one complete new Aether program that solves the task below. Treat this as a fresh, "
    "unrelated task: do not reuse code, variable names, helper functions, or structure "
    "from your earlier answers in this conversation."
)


def build_followup_prompt(task: "adb.Task") -> str:
    return textwrap.dedent(
        f"""\
        {RESET_INSTRUCTION}

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


def build_followup_repair_prompt(
    task: "adb.Task",
    previous_source: str,
    attempt_number: int,
    failure_summary: str,
    observed_stdout: str,
    observed_stderr: str,
) -> str:
    # Deliberately NOT prefixed with RESET_INSTRUCTION: a repair follow-up should
    # reference the model's own immediately-prior answer, not disclaim it.
    return textwrap.dedent(
        f"""\
        Your previous attempt at this same task did not satisfy the benchmark task.
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


class ChatSession:
    """openai_chat_completions destinations: no server-side state, so we resend
    the whole growing messages list every turn. Only saves anything if the
    provider caches the repeated prefix -- we can't confirm that from here, but
    wire_prompt_tokens (recorded per attempt) lets you check after the fact
    whether elapsed time/cost scaled with cumulative session size."""

    def __init__(self, destination: "adb.Destination") -> None:
        self.destination = destination
        self.messages: list[dict[str, str]] = []

    def turn(self, text: str) -> dict[str, Any]:
        self.messages.append({"role": "user", "content": text})
        wire_tokens = sum(adb.approx_tokens(m["content"]) for m in self.messages)
        result = adb.invoke_openai_chat_completions_messages(list(self.messages), self.destination)
        self.messages.append({"role": "assistant", "content": result["raw_text"]})
        result["wire_prompt_tokens"] = wire_tokens
        return result


class ResponsesSession:
    """openai_responses destinations: true server-side state via previous_response_id.
    Only the new turn's text is ever sent after the first -- the guide is never
    retransmitted once threaded server-side."""

    def __init__(self, destination: "adb.Destination") -> None:
        self.destination = destination
        self.previous_response_id: str | None = None

    def turn(self, text: str) -> dict[str, Any]:
        wire_tokens = adb.approx_tokens(text)
        result = adb.invoke_openai_responses_session(
            text, self.destination, previous_response_id=self.previous_response_id
        )
        self.previous_response_id = result.get("response_id") or self.previous_response_id
        result["wire_prompt_tokens"] = wire_tokens
        return result


def make_session(destination: "adb.Destination"):
    if destination.kind == "openai_responses":
        return ResponsesSession(destination)
    if destination.kind == "openai_chat_completions":
        return ChatSession(destination)
    raise RuntimeError(f"session mode does not support destination kind {destination.kind!r}")


def evaluate_attempt_session(
    session: Any,
    prompt_text: str,
    prompt_kind: str,
    task: "adb.Task",
    args: argparse.Namespace,
    cumulative_tokens_before: int,
) -> dict[str, Any]:
    attempt: dict[str, Any] = {"prompt_kind": prompt_kind, "prompt_approx_tokens": adb.approx_tokens(prompt_text)}
    generation = session.turn(prompt_text)
    wire_tokens = generation.pop("wire_prompt_tokens", 0)
    attempt["wire_prompt_tokens"] = wire_tokens
    attempt["cumulative_context_tokens_before"] = cumulative_tokens_before
    source_code = adb.sanitize_code(generation["raw_text"])
    attempt["generation"] = generation
    attempt["usage"] = adb.normalize_usage(generation.get("usage"))
    attempt["generated_ok"] = bool(source_code.strip())
    attempt["source_code"] = source_code
    attempt["source_approx_tokens"] = adb.approx_tokens(source_code) if source_code.strip() else 0
    if attempt["generated_ok"]:
        attempt["run"] = adb.compile_and_run(task, source_code, args)
    else:
        attempt["run"] = {
            "returncode": -1,
            "stdout": "",
            "stderr": "empty model output",
            "elapsed_seconds": 0.0,
            "exact_stdout_match": False,
        }
    return attempt


def run_task_session(
    session: Any,
    task: "adb.Task",
    is_first_task: bool,
    doc_name: str,
    doc_text: str,
    args: argparse.Namespace,
) -> dict[str, Any]:
    cumulative_before = getattr(session, "_cumulative_tokens", 0)
    prompt = adb.build_prompt(doc_name, doc_text, task) if is_first_task else build_followup_prompt(task)
    attempt = evaluate_attempt_session(session, prompt, "initial", task, args, cumulative_before)
    attempts = [attempt]

    if args.repair_attempts > 0 and not attempt["run"]["exact_stdout_match"]:
        for repair_index in range(args.repair_attempts):
            failure_summary = adb.derive_failure_summary(
                generated_ok=attempt.get("generated_ok", False),
                run=attempt["run"],
                generation_error=attempt.get("generation_error"),
                expected_stdout=task.expected_stdout,
            )
            repair_prompt = build_followup_repair_prompt(
                task=task,
                previous_source=adb.truncate_for_prompt(attempt.get("source_code", ""), args.repair_feedback_limit),
                attempt_number=repair_index + 1,
                failure_summary=failure_summary,
                observed_stdout=adb.truncate_for_prompt(attempt["run"].get("stdout", ""), args.repair_feedback_limit),
                observed_stderr=adb.truncate_for_prompt(attempt["run"].get("stderr", ""), args.repair_feedback_limit),
            )
            cumulative_before = getattr(session, "_cumulative_tokens", 0)
            attempt = evaluate_attempt_session(session, repair_prompt, "repair", task, args, cumulative_before)
            attempts.append(attempt)
            if attempt["run"]["exact_stdout_match"]:
                break

    session._cumulative_tokens = getattr(session, "_cumulative_tokens", 0) + sum(
        a["wire_prompt_tokens"] + a["source_approx_tokens"] for a in attempts
    )

    case_record = dict(attempts[-1])
    case_record["task_id"] = task.task_id
    case_record["task_title"] = task.title
    case_record["attempts"] = attempts
    case_record["attempt_count"] = len(attempts)
    case_record["resolved_after_repair"] = case_record["run"]["exact_stdout_match"] and len(attempts) > 1
    case_record["failure_fingerprint"] = (
        "" if case_record["run"]["exact_stdout_match"] else adb.derive_failure_fingerprint(case_record)
    )
    return case_record


def run_destination_session(
    destination: "adb.Destination",
    doc_name: str,
    doc_text: str,
    tasks: list["adb.Task"],
    args: argparse.Namespace,
) -> dict[str, Any]:
    session = make_session(destination)
    results = []
    for index, task in enumerate(tasks):
        if args.progress:
            print(f"[session] {destination.destination_id} {doc_name} turn={index} task={task.task_id} start",
                  file=sys.stderr, flush=True)
        try:
            case_record = run_task_session(session, task, index == 0, doc_name, doc_text, args)
        except Exception as exc:  # noqa: BLE001 - surfaced in JSON report
            case_record = {
                "task_id": task.task_id,
                "task_title": task.title,
                "generated_ok": False,
                "generation_error": str(exc),
                "run": {"returncode": -1, "stdout": "", "stderr": str(exc), "elapsed_seconds": 0.0, "exact_stdout_match": False},
                "attempts": [],
                "attempt_count": 0,
                "resolved_after_repair": False,
                "failure_fingerprint": f"generation:{exc}",
            }
        if args.progress:
            run = case_record.get("run", {})
            print(
                f"[session] {destination.destination_id} {doc_name} turn={index} task={task.task_id} "
                f"exact={int(bool(run.get('exact_stdout_match', False)))} "
                f"wire_tok={case_record.get('attempts', [{}])[0].get('wire_prompt_tokens', '?') if case_record.get('attempts') else '?'}",
                file=sys.stderr, flush=True,
            )
        results.append(case_record)
    return {
        "destination_id": destination.destination_id,
        "type": destination.kind,
        "model": destination.model,
        "doc_name": doc_name,
        "results": results,
        "summary": adb.summarize(results),
        "failure_patterns": adb.summarize_failure_patterns(results),
    }


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks", type=pathlib.Path, required=True)
    parser.add_argument("--destinations-config", type=pathlib.Path, required=True)
    parser.add_argument("--destination", action="append", default=[])
    parser.add_argument("--docs", default="full", choices=("full", "small"))
    parser.add_argument("--repair-attempts", type=int, default=0)
    parser.add_argument("--repair-feedback-limit", type=int, default=1200)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=adb.DEFAULT_AETHER_BIN)
    parser.add_argument("--sandbox-deny", default="net,proc")
    parser.add_argument("--output-json", type=pathlib.Path, default=None)
    parser.add_argument("--text-summary", action="store_true")
    parser.add_argument("--progress", action="store_true")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    tasks = adb.load_tasks(args.tasks)
    destinations = adb.load_destinations(args.destinations_config)
    if args.destination:
        wanted = set(args.destination)
        destinations = [d for d in destinations if d.destination_id in wanted]

    doc_variants = adb.resolve_docs([args.docs])
    doc_name, doc_path = doc_variants[0]
    doc_text = adb.read_text(doc_path) if doc_path else ""

    aether_version, aether_version_raw = adb.capture_aether_version(args.aether_bin)

    report: dict[str, Any] = {
        "tasks_file": str(args.tasks),
        "destinations_config": str(args.destinations_config),
        "doc_name": doc_name,
        "aether_version": aether_version,
        "aether_version_raw": aether_version_raw,
        "aether_bin": str(args.aether_bin),
        "mode": "interactive_session",
        "reset_instruction": RESET_INSTRUCTION,
        "destinations": [],
    }

    for destination in destinations:
        dest_report = run_destination_session(destination, doc_name, doc_text, tasks, args)
        report["destinations"].append(dest_report)
        if args.output_json:
            adb.write_json_atomic(args.output_json, report)
        if args.text_summary:
            s = dest_report["summary"]
            print(f"destination   : {dest_report['destination_id']}")
            print(f"model         : {dest_report['model']}")
            print(
                f" {doc_name}  generated={s['generated_ok']}/{s['total_cases']}  "
                f"run={s['run_ok']}/{s['total_cases']}  exact={s['exact_stdout_match']}/{s['total_cases']}  "
                f"repaired={s['resolved_after_repair']}/{s['total_cases']}"
            )
            for f in dest_report["failure_patterns"]:
                print(f"       fail x{f['count']}: {f['fingerprint']} [tasks: {', '.join(f['task_ids'])}]")
            print("")

    if args.output_json:
        adb.write_json_atomic(args.output_json, report)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
