#!/usr/bin/env python3
"""Generative Aether idea-miner — the no-oracle, free-form sibling of the bench.

Where ``tools/aether_doc_bench.py`` measures pass/fail on a *fixed* task list,
this harness lets each model write whatever Aether it finds interesting and
watches where the language disappoints it. That signal is the most direct input
to Aether's design philosophy: *when a capable model consistently reaches for a
construct that does not exist — or trips on one that does — that is a language
bug.* (See ``components/aether/docs/ideas_and_todo.md`` and the benchmark
write-ups ``aether_guided_benchmark.md`` / ``aether_specialization_findings.md``.)

Per LLM destination the loop is:

  1. Inject the Aether guide into the prompt — the **concise** guide for
     small-context/small models, the **full** guide for large-context ones
     (``--guide auto`` picks; override with ``--guide full|small`` or a per-
     destination ``"guide"`` field).
  2. Ask the model to invent several *interesting / useful / novel* Aether
     programs **of its own choosing** (open-ended, no task list), each with a
     stated intent. Variety is encouraged (algorithms, data processing, records
     + methods, TOON/JSON parsing, @pure/@pre/@post contracts, fx-scoped
     effects).
  3. Extract every program and compile + run it with the gating ``aether``
     binary (``aether --no-cache prog.aether``).
  4. On a compile/runtime error, feed the compiler's *coded* diagnostic
     (FX-001, SCOPE-001, TOON-001, ...) back and let the model self-correct
     (the same repair loop the bench uses).
  5. EVAL has **no oracle**: "success" = compiles and runs cleanly (return code
     0). The product is the *failure* analysis — every error code plus the
     offending construct, especially recurring "reached for X that Aether lacks"
     (SCOPE-001 on an undeclared name) and "tripped on Y that exists" (a code a
     model keeps hitting). Findings are aggregated across models and ranked by
     how many DISTINCT models hit them.

Output: a JSON report (every program + attempt + diagnostic), a ranked Markdown
findings report, and an optional append of the actionable findings to the
standing backlog at ``components/aether/docs/ideas_and_todo.md``.

All the model plumbing (guide injection, OpenAI-compatible chat calls with
extra_headers/extra_body, reasoning-block stripping, byte-level-BPE decode, the
deadline-guarded request, ``--diagnostics-json`` extraction, context-limit
detection, destination loading) is *imported* from ``aether_doc_bench`` so a fix
there (e.g. a new reasoning-model output format) propagates here for free.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import pathlib
import re
import sys
import textwrap
import threading
import time
import urllib.request
from urllib.parse import urlparse
from typing import Any

# Reuse the fixed-task harness's plumbing. It guards main() under
# ``if __name__ == "__main__"`` so importing it is side-effect-free.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_doc_bench as adb  # noqa: E402


REPO_ROOT = adb.REPO_ROOT
OUTPUT_END_MARKER = adb.OUTPUT_END_MARKER
# The gating binary the brief specifies (SDL build). Resolved to absolute below
# because compile_and_run runs from a temp cwd — a relative path silently fails.
DEFAULT_AETHER_BIN = REPO_ROOT / "components" / "aether" / "build-sdl" / "aether"
FULL_GUIDE = adb.DOC_VARIANTS["full"]
CONCISE_GUIDE = adb.DOC_VARIANTS["small"]
DEFAULT_DESTINATIONS = REPO_ROOT / "Tests" / "aether_doc_bench" / "destinations.m5t.json"
DEFAULT_IDEAS_FILE = REPO_ROOT / "components" / "aether" / "docs" / "ideas_and_todo.md"

# Block markers the model is asked to wrap each program in. Chosen to be
# unambiguous and regex-friendly, and to survive models that ALSO add Markdown
# fences inside the block (clean_source strips a wrapping fence).
PROGRAM_BEGIN = "<<<AETHER-PROGRAM>>>"
SOURCE_BEGIN = "<<<SOURCE>>>"
PROGRAM_END = "<<<END-PROGRAM>>>"

# Guide auto-selection thresholds: a small loaded context OR a small model gets
# the concise guide; everything else gets the full guide.
SMALL_CONTEXT_TOKEN_THRESHOLD = 16384
SMALL_MODEL_B_THRESHOLD = 9.0

# Inspiration only — the model is told these are NOT a checklist; it should
# write what it genuinely finds useful and reach for whatever it expects to exist.
SUGGESTED_AREAS = [
    "a non-trivial algorithm (sorting, search, dynamic programming, number theory, string work)",
    "data processing over a literal dataset (group / aggregate / filter / rank)",
    "a record type with methods — a small typed model that carries behavior",
    "parsing TOON or JSON input with the toon_* builtins and pulling fields out",
    "design-by-contract: @pure helpers guarded by @pre / @post conditions",
    "effects discipline: fx-scoped output/IO with the pure computation kept outside fx",
    "a small state machine, simulation, or interpreter",
    "text formatting or a generated report",
]


# --------------------------------------------------------------------------- #
# Guide selection
# --------------------------------------------------------------------------- #
def choose_guide_name(
    destination: adb.Destination,
    override: str,
    per_destination_hint: str | None,
) -> str:
    """Return 'full' or 'small' for this destination.

    Precedence: explicit CLI ``--guide`` > per-destination ``"guide"`` hint >
    auto. Auto picks the concise guide when the loaded context is small or the
    model is small (<= ~9B), else the full guide.
    """
    if override in ("full", "small"):
        return override
    if per_destination_hint in ("full", "small"):
        return per_destination_hint

    try:
        limit = adb.get_destination_context_limit(destination)
    except Exception:
        limit = None
    if limit is not None and limit < SMALL_CONTEXT_TOKEN_THRESHOLD:
        return "small"

    size_b = adb.infer_model_size_billions(destination.model)
    if size_b is not None and size_b <= SMALL_MODEL_B_THRESHOLD:
        return "small"
    return "full"


def guide_path_for(name: str) -> pathlib.Path:
    return FULL_GUIDE if name == "full" else CONCISE_GUIDE


# --------------------------------------------------------------------------- #
# Prompts
# --------------------------------------------------------------------------- #
def build_generation_prompt(
    guide_name: str,
    guide_text: str,
    n_programs: int,
    avoid_intents: list[str],
) -> str:
    areas = "\n".join(f"  - {a}" for a in SUGGESTED_AREAS)
    avoid_block = ""
    if avoid_intents:
        joined = "\n".join(f"  - {i}" for i in avoid_intents if i)
        if joined:
            avoid_block = (
                "\nYou (or another model) already wrote programs with these intents; "
                "pick DIFFERENT ideas this round:\n" + joined + "\n"
            )
    return textwrap.dedent(
        f"""\
        You are an expert Aether programmer exploring what the language can do.

        {adb.build_guide_block(guide_name, guide_text)}

        Write {n_programs} short, COMPLETE Aether programs of your own choosing —
        whatever you find genuinely interesting, useful, or novel. There is no
        fixed task: you decide what each program does. Aim for VARIETY across the
        set. For inspiration (not a checklist — ignore freely):
        {areas}
        {avoid_block}
        Write naturally. If you expect a builtin, keyword, or construct to exist,
        USE it the way you would reach for it — do not avoid a feature just
        because you are unsure it is supported. Each program should be small
        enough to compile and run on its own and should print something.

        For EACH program, output exactly this block and nothing else around it:

        {PROGRAM_BEGIN}
        INTENT: one sentence describing what this program does and why it is interesting
        {SOURCE_BEGIN}
        <the complete Aether program as raw source — no Markdown fences>
        {PROGRAM_END}

        Rules:
        - Produce exactly {n_programs} such blocks, back to back.
        - Raw Aether source inside the block. Do not wrap it in ``` fences.
        - Do not explain anything outside the blocks.
        - After the final {PROGRAM_END}, output a line containing exactly {OUTPUT_END_MARKER}
        """
    )


def build_repair_prompt(
    guide_name: str,
    guide_text: str,
    intent: str,
    previous_source: str,
    attempt_number: int,
    failure_summary: str,
    observed_stdout: str,
    observed_stderr: str,
) -> str:
    return textwrap.dedent(
        f"""\
        You are repairing a failed Aether program that you wrote.

        {adb.build_guide_block(guide_name, guide_text)}

        Keep the program's original intent, but make it compile and run cleanly
        with the local `aether` compiler (exit code 0). Return ONE corrected,
        complete Aether program.

        Requirements:
        - Return only raw Aether source code.
        - Do not wrap the answer in Markdown fences.
        - Do not explain the code.
        - After the full program, output a final line containing exactly {OUTPUT_END_MARKER}

        Original intent:
        {intent or "(unspecified)"}

        Repair attempt number:
        {attempt_number}

        Failure summary (the compiler's coded diagnostic — its code maps to a
        section of the Aether guide above):
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


# --------------------------------------------------------------------------- #
# Program extraction (multi-program, robust to format drift)
# --------------------------------------------------------------------------- #
_FENCE_RE = re.compile(r"```[ \t]*[A-Za-z0-9_.+-]*[ \t]*\r?\n(.*?)\r?\n[ \t]*```", re.S)
_LANG_TAGS = ("aether", "rust", "python", "c", "go", "text", "")


def clean_source(raw: str) -> str:
    """Trim a single program body: decode byte-level artifacts, peel a wrapping
    Markdown fence, and drop a stray leading bare language tag. Conservative —
    only strips a fence that WRAPS the whole body (starts with ```)."""
    text = adb.decode_bytelevel_artifacts(raw).strip()
    lines = text.splitlines()
    if lines and lines[0].lstrip().startswith("```"):
        lines = lines[1:]
        while lines and lines[-1].strip() == "```":
            lines.pop()
        text = "\n".join(lines).strip()
        lines = text.splitlines()
    if lines and lines[0].strip().lower() in _LANG_TAGS and not lines[0].strip().startswith("//"):
        if lines[0].strip().lower() in ("aether", "rust", "python", "c", "go", "text"):
            text = "\n".join(lines[1:]).strip()
    return text


def _split_intent_source(block: str) -> tuple[str, str]:
    intent = ""
    si = block.find(SOURCE_BEGIN)
    if si != -1:
        head = block[:si]
        source = block[si + len(SOURCE_BEGIN):]
        m = re.search(r"INTENT:\s*(.*)", head)
        if m:
            intent = m.group(1).strip()
        return intent, source
    # No SOURCE marker: treat an INTENT: line (if any) as the intent and the
    # rest as source.
    m = re.search(r"INTENT:\s*(.*)", block)
    if m:
        intent = m.group(1).strip()
        source = block[:m.start()] + block[m.end():]
        return intent, source
    return "", block


def extract_programs(raw_text: str) -> list[dict[str, str]]:
    """Pull (intent, source) pairs out of a model reply. Tries the block format
    first, then falls back to fenced blocks, then to the whole reply as one
    program — so even a model that ignores the format still contributes."""
    text = adb.strip_reasoning_block(raw_text)
    idx = text.find(OUTPUT_END_MARKER)
    if idx != -1:
        text = text[:idx]

    programs: list[dict[str, str]] = []

    # Primary: fully-delimited blocks.
    block_re = re.compile(re.escape(PROGRAM_BEGIN) + r"(.*?)" + re.escape(PROGRAM_END), re.S)
    for block in block_re.findall(text):
        intent, source = _split_intent_source(block)
        source = clean_source(source)
        if source.strip():
            programs.append({"intent": intent, "source_code": source, "extract": "block"})
    if programs:
        return programs

    # Fallback A: BEGIN markers present but END markers dropped — split on BEGIN.
    if PROGRAM_BEGIN in text:
        for chunk in text.split(PROGRAM_BEGIN)[1:]:
            intent, source = _split_intent_source(chunk)
            source = clean_source(source)
            if source.strip():
                programs.append({"intent": intent, "source_code": source, "extract": "begin_only"})
        if programs:
            return programs

    # Fallback B: Markdown-fenced code blocks.
    for m in _FENCE_RE.finditer(text):
        source = clean_source(m.group(1))
        if source.strip():
            programs.append({"intent": "(unspecified)", "source_code": source, "extract": "fence"})
    if programs:
        return programs

    # Last resort: the whole reply is one program.
    whole = adb.sanitize_code(raw_text)
    if whole.strip():
        programs.append({"intent": "(unspecified)", "source_code": whole, "extract": "whole"})
    return programs


# --------------------------------------------------------------------------- #
# Generation dispatch: T'ra resource scheduler vs direct provider
# --------------------------------------------------------------------------- #
def invoke_tra_scheduler(prompt: str, destination: adb.Destination) -> dict[str, Any]:
    """Folded into ``adb.invoke_tra_queue`` -- the single canonical T'Ra queue adapter
    (explain-validate -> idempotent retried submit -> long-poll -> cancel-on-giveup),
    shared with the doc-bench so there is ONE implementation, not two divergent ones.
    This thin entrypoint only defaults the miner's submitter label; ``"type":
    "tra_scheduler"`` destinations are otherwise handled exactly like the bench's
    ``"tra_queue"`` (the dispatch in adb.run_model treats the two kinds identically).
    """
    eb = {"submitter": "aether_idea_miner", **(destination.extra_body or {})}
    return adb.invoke_tra_queue(prompt, dataclasses.replace(destination, extra_body=eb))


def generate(prompt: str, destination: adb.Destination) -> dict[str, Any]:
    """Dispatch one generation: the T'ra scheduler (coordinated, polled) for
    ``tra_scheduler`` destinations, else the bench's deadline-guarded direct call."""
    if destination.kind == "tra_scheduler":
        return invoke_tra_scheduler(prompt, destination)
    return adb.run_model_with_deadline(prompt, destination)


# --------------------------------------------------------------------------- #
# Compile + run (reuse adb.compile_and_run via a synthetic, oracle-free Task)
# --------------------------------------------------------------------------- #
def compile_program(source_code: str, aether_bin: pathlib.Path, timeout_seconds: int,
                     sandbox_deny: str = "net,proc") -> dict[str, Any]:
    task = adb.Task(
        task_id="idea",
        title="generative",
        prompt="",
        expected_stdout="",  # no oracle — success is rc == 0, not a stdout match
        timeout_seconds=timeout_seconds,
        cwd=None,
        files=None,
    )
    ns = argparse.Namespace(aether_bin=aether_bin, sandbox_deny=sandbox_deny)
    return adb.compile_and_run(task, source_code, ns)


def run_brief(run: dict[str, Any], stdout_cap: int = 2000, stderr_cap: int = 4000) -> dict[str, Any]:
    """A storage-friendly copy of a run result (caps stdout/stderr, keeps the
    full structured diagnostics)."""
    out = dict(run)
    if isinstance(out.get("stdout"), str) and len(out["stdout"]) > stdout_cap:
        out["stdout"] = out["stdout"][:stdout_cap] + "\n...[truncated]..."
    if isinstance(out.get("stderr"), str) and len(out["stderr"]) > stderr_cap:
        out["stderr"] = out["stderr"][:stderr_cap] + "\n...[truncated]..."
    return out


# --------------------------------------------------------------------------- #
# Failure analysis
# --------------------------------------------------------------------------- #
def primary_diagnostic(run: dict[str, Any]) -> dict[str, Any] | None:
    """The diagnostic that best describes the failure: first one carrying a code
    (the compiler pairs a code with a code=null 'help:' line), else the first
    error-severity diagnostic, else the first."""
    diags = run.get("diagnostics") or []
    diags = [d for d in diags if isinstance(d, dict)]
    for d in diags:
        if d.get("code"):
            return d
    for d in diags:
        if d.get("severity") == "error" and (d.get("message") or "").strip():
            return d
    return diags[0] if diags else None


# Runtime failures Aether reports on STDOUT (nonzero exit, empty stderr, no
# diagnostics) — e.g. a legitimately-violated contract prints ``Aether @post
# failed in <fn>``. Without scanning stdout these are indistinguishable from a
# genuine silent crash and pollute the "silent failure" finding category.
RUNTIME_STDOUT_ERROR_PREFIXES = (
    "Aether @post failed",
    "Aether @pre failed",
    "Runtime Error",
    "Compiler error",
)


def first_stdout_error_line(stdout: Any) -> str | None:
    """The first stdout line reporting a known Aether runtime/compile error
    (see ``RUNTIME_STDOUT_ERROR_PREFIXES``), or None. Used to reclassify a
    failure that was reported on stdout rather than stderr."""
    if not isinstance(stdout, str) or not stdout:
        return None
    for line in stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith(RUNTIME_STDOUT_ERROR_PREFIXES):
            return stripped
    return None


def analyze_failure(source_code: str, run: dict[str, Any]) -> dict[str, Any] | None:
    """Return a structured failure record, or None if the program ran cleanly."""
    if run.get("returncode", -1) == 0:
        return None

    diag = primary_diagnostic(run)
    code = phase = kind = message = diag_line = None
    if diag:
        code = diag.get("code")
        phase = diag.get("phase")
        kind = diag.get("kind")
        message = (diag.get("message") or "").strip() or None
        diag_line = diag.get("line")

    stderr = (run.get("stderr") or "").strip()
    if not message and stderr:
        message = stderr.splitlines()[0]

    # No coded diagnostic and nothing on stderr, but the program still failed:
    # some Aether runtime errors (violated @pre/@post contracts, raised runtime
    # errors) print to STDOUT instead. Treat a known error line there as the
    # reported failure so it fingerprints as runtime, not "silent".
    stdout_error_line = None
    if not message and not stderr:
        stdout_error_line = first_stdout_error_line(run.get("stdout"))
        if stdout_error_line:
            message = stdout_error_line
            if phase is None:
                phase = "runtime"

    identifier = None
    if message:
        m = re.search(r"'([^']+)'", message)
        if m:
            identifier = m.group(1)

    offending_line = None
    if isinstance(diag_line, int) and diag_line >= 1:
        src_lines = source_code.splitlines()
        if diag_line <= len(src_lines):
            offending_line = src_lines[diag_line - 1].strip()

    # "Reached for X that Aether lacks": a name the compiler could not resolve.
    is_missing = bool(
        code == "SCOPE-001" and message and "not in scope" in message and identifier
    )

    return {
        "returncode": run.get("returncode"),
        "code": code,
        "phase": phase,
        "kind": kind,
        "message": message,
        "diag_line": diag_line,
        "offending_identifier": identifier,
        "offending_line": offending_line,
        "is_missing_identifier": is_missing,
        "stderr_head": stderr.splitlines()[0] if stderr else (stdout_error_line or ""),
        "silent": bool(diag is None and not stderr and not stdout_error_line),
    }


def finding_key(f: dict[str, Any]) -> str:
    """Group failures into findings. Missing-name failures key by the name
    (so the same absent builtin clusters across models); coded failures key by
    code; runtime/silent failures key by a normalized message."""
    if f.get("is_missing_identifier"):
        return f"missing:{f['offending_identifier']}"
    if f.get("code"):
        return f"code:{f['code']}"
    if f.get("phase") == "runtime" and f.get("message"):
        norm = re.sub(r"\d+", "N", f["message"])
        return f"runtime:{norm[:80]}"
    if f.get("silent"):
        return f"silent:returncode={f.get('returncode')}"
    if f.get("stderr_head"):
        return f"stderr:{f['stderr_head'][:80]}"
    return f"other:returncode={f.get('returncode')}"


def source_excerpt(source: str, line: Any, context: int = 2) -> str:
    if not source:
        return ""
    lines = source.splitlines()
    if not isinstance(line, int) or line < 1 or line > len(lines):
        head = [ln for ln in lines if ln.strip()][:6]
        return "\n".join(head)
    lo = max(0, line - 1 - context)
    hi = min(len(lines), line + context)
    out = []
    for i in range(lo, hi):
        marker = ">>" if i == line - 1 else "  "
        out.append(f"{marker} {lines[i]}")
    return "\n".join(out)


# --------------------------------------------------------------------------- #
# Per-program processing (initial attempt + repair loop)
# --------------------------------------------------------------------------- #
def process_program(
    prog: dict[str, str],
    destination: adb.Destination,
    guide_name: str,
    guide_text: str,
    args: argparse.Namespace,
) -> dict[str, Any]:
    source = prog["source_code"]
    intent = prog.get("intent", "")
    record: dict[str, Any] = {
        "intent": intent,
        "extract": prog.get("extract"),
        "initial_source": source,
        "attempts": [],
    }

    run = compile_program(source, args.aether_bin, args.timeout_seconds, args.sandbox_deny)
    initial_failure = analyze_failure(source, run)
    record["attempts"].append({
        "kind": "initial",
        "source_code": source,
        "run": run_brief(run),
        "failure": initial_failure,
    })
    record["initial_failure"] = initial_failure

    final_run = run
    fixed = False
    if initial_failure is not None and args.repair_attempts > 0:
        cur_run = run
        cur_source = source
        for i in range(args.repair_attempts):
            failure_summary = adb.derive_failure_summary(generated_ok=True, run=cur_run)
            prompt = build_repair_prompt(
                guide_name=guide_name,
                guide_text=guide_text,
                intent=intent,
                previous_source=adb.truncate_for_prompt(cur_source, args.repair_feedback_limit),
                attempt_number=i + 1,
                failure_summary=failure_summary,
                observed_stdout=adb.truncate_for_prompt(cur_run.get("stdout", ""), args.repair_feedback_limit),
                observed_stderr=adb.truncate_for_prompt(cur_run.get("stderr", ""), args.repair_feedback_limit),
            )
            attempt: dict[str, Any] = {"kind": "repair", "attempt_number": i + 1}
            try:
                generation = generate(prompt, destination)
                new_source = adb.sanitize_code(generation.get("raw_text", ""))
                attempt["usage"] = adb.normalize_usage(generation.get("usage"))
            except Exception as exc:  # provider error / timeout — record and stop
                attempt["generation_error"] = str(exc)
                attempt["source_code"] = ""
                attempt["run"] = run_brief({
                    "returncode": -1, "stdout": "", "stderr": str(exc),
                    "elapsed_seconds": 0.0, "exact_stdout_match": False, "diagnostics": None,
                })
                attempt["failure"] = analyze_failure("", attempt["run"])
                record["attempts"].append(attempt)
                break

            if not new_source.strip():
                attempt["source_code"] = ""
                attempt["generation_error"] = "empty repair output"
                attempt["run"] = run_brief({
                    "returncode": -1, "stdout": "", "stderr": "empty repair output",
                    "elapsed_seconds": 0.0, "exact_stdout_match": False, "diagnostics": None,
                })
                attempt["failure"] = analyze_failure("", attempt["run"])
                record["attempts"].append(attempt)
                break

            new_run = compile_program(new_source, args.aether_bin, args.timeout_seconds, args.sandbox_deny)
            attempt["source_code"] = new_source
            attempt["run"] = run_brief(new_run)
            attempt["failure"] = analyze_failure(new_source, new_run)
            record["attempts"].append(attempt)

            cur_run = new_run
            cur_source = new_source
            final_run = new_run
            if new_run.get("returncode", -1) == 0:
                fixed = True
                break

    record["success"] = final_run.get("returncode", -1) == 0
    record["fixed_by_repair"] = fixed
    record["final_returncode"] = final_run.get("returncode")
    return record


# --------------------------------------------------------------------------- #
# Mining: aggregate failures into ranked findings
# --------------------------------------------------------------------------- #
def mine_findings(model_records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    findings: dict[str, dict[str, Any]] = {}
    for mr in model_records:
        did = mr["destination_id"]
        model = mr.get("model") or did
        for rec in mr.get("programs", []):
            f = rec.get("initial_failure")
            if not f:
                continue
            key = finding_key(f)
            if f.get("is_missing_identifier"):
                kind = "missing_construct"
            elif f.get("code"):
                kind = "error_code"
            elif f.get("phase") == "runtime":
                kind = "runtime"
            elif f.get("silent"):
                kind = "silent"
            else:
                kind = "other"
            entry = findings.setdefault(key, {
                "key": key,
                "kind": kind,
                "code": f.get("code"),
                "identifier": f.get("offending_identifier") if f.get("is_missing_identifier") else None,
                "_models": set(),
                "_destinations": set(),
                "occurrences": 0,
                "unrepaired": 0,
                "messages": set(),
                "examples": [],
            })
            entry["_models"].add(model)
            entry["_destinations"].add(did)
            entry["occurrences"] += 1
            if not rec.get("fixed_by_repair"):
                entry["unrepaired"] += 1
            if f.get("message"):
                entry["messages"].add(f["message"])
            if len(entry["examples"]) < 3:
                entry["examples"].append({
                    "destination_id": did,
                    "model": model,
                    "intent": rec.get("intent", ""),
                    "code": f.get("code"),
                    "message": f.get("message"),
                    "diag_line": f.get("diag_line"),
                    "offending_line": f.get("offending_line"),
                    "source_excerpt": source_excerpt(rec.get("initial_source", ""), f.get("diag_line")),
                    "fixed_by_repair": rec.get("fixed_by_repair", False),
                })

    out: list[dict[str, Any]] = []
    for e in findings.values():
        models = sorted(e.pop("_models"))
        dests = sorted(e.pop("_destinations"))
        e["models"] = models
        e["model_count"] = len(models)
        e["destinations"] = dests
        e["messages"] = sorted(e["messages"])[:5]
        out.append(e)
    # Rank by breadth (distinct models) first, then total occurrences.
    out.sort(key=lambda e: (-e["model_count"], -e["occurrences"], e["key"]))
    return out


def load_code_glossary(guide_path: pathlib.Path | None) -> dict[str, str]:
    """Parse ``N. **CODE.** gloss...`` lines out of a guide. The coded errors
    double as the guide's own section headings, so this maps a diagnostic code
    to the one-line rule it points back to."""
    gloss: dict[str, str] = {}
    if not guide_path or not guide_path.exists():
        return gloss
    code_re = re.compile(r"\s*\d+\.\s+\*\*([A-Z]{2,6}-\d{3})\.\*\*\s+(.*)")
    for ln in guide_path.read_text(encoding="utf-8").splitlines():
        m = code_re.match(ln)
        if m and m.group(1) not in gloss:
            gloss[m.group(1)] = m.group(2).strip().rstrip(":")
    return gloss


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #
def finding_headline(f: dict[str, Any]) -> str:
    if f["kind"] == "missing_construct":
        return f"Reached for `{f['identifier']}` — not in scope (does not exist)"
    if f["kind"] == "error_code":
        return f"Tripped on `{f['code']}`"
    if f["kind"] == "runtime":
        return "Runtime error"
    if f["kind"] == "silent":
        return "Silent failure (nonzero exit, no diagnostic)"
    return f["key"]


def suggested_action(f: dict[str, Any]) -> str:
    if f["kind"] == "missing_construct":
        if f["unrepaired"] >= max(1, f["occurrences"]):
            return ("Candidate **language gap**: add the builtin/construct, or add a guide entry "
                    "steering models to the existing equivalent. Repair did not rescue it.")
        return ("Likely **guide clarification**: the equivalent exists; models guess the wrong name. "
                "Repair often fixed it once pointed at the diagnostic.")
    if f["kind"] == "error_code":
        return ("Recurring trip-up on an existing rule — candidate **guide clarification** (make the "
                "rule harder to miss) or a friendlier diagnostic.")
    if f["kind"] == "runtime":
        return "Models write compiling-but-crashing code here — usually program logic, not a language gap."
    if f["kind"] == "silent":
        return ("**Diagnostic gap**: the program fails with no coded error or stderr. The compiler should "
                "emit a diagnostic for this shape.")
    return "Review."


def render_markdown(report: dict[str, Any], glossary: dict[str, str]) -> str:
    s = report["summary"]
    lines: list[str] = []
    lines.append("# Aether generative idea-mining — findings")
    lines.append("")
    lines.append(f"*Generated {report['generated_at']} by `tools/aether_idea_miner.py` "
                 f"against `aether` {report['aether_version']}.*")
    lines.append("")
    lines.append("Each model freely wrote Aether programs of its own choosing; this is where the "
                 "language disappointed it. No oracle — success means the program compiled and ran "
                 "(exit 0). Findings are ranked by how many DISTINCT models hit them.")
    lines.append("")
    lines.append("## Run summary")
    lines.append("")
    lines.append(f"- Models: **{s['model_count']}** ({s['models_ok']} produced programs)")
    lines.append(f"- Programs generated: **{s['programs_total']}**")
    lines.append(f"- Compiled+ran cleanly: **{s['programs_success']}** "
                 f"({pct(s['programs_success'], s['programs_total'])})")
    lines.append(f"- Failed initially, rescued by repair: **{s['fixed_by_repair']}**")
    lines.append(f"- Distinct findings: **{s['finding_count']}**")
    lines.append("")

    # Per-model line.
    lines.append("| destination | model | guide | programs | ran clean | fixed | failed |")
    lines.append("|---|---|---|---|---|---|---|")
    for mr in report["models"]:
        ms = mr.get("stats", {})
        note = "" if not mr.get("error") else f" ⚠️ {mr['error'][:40]}"
        lines.append(
            f"| {mr['destination_id']} | {mr.get('model') or '-'} | {mr.get('guide','-')} | "
            f"{ms.get('programs', 0)} | {ms.get('success', 0)} | {ms.get('fixed', 0)} | "
            f"{ms.get('failed', 0)}{note} |"
        )
    lines.append("")

    findings = report["findings"]
    if not findings:
        lines.append("## Findings")
        lines.append("")
        lines.append("No failures recorded — every generated program compiled and ran. "
                     "(Try a larger `--programs-per-model`, more/weaker models, or a higher "
                     "`--temperature` for more variety.)")
        return "\n".join(lines) + "\n"

    lines.append("## Ranked findings")
    lines.append("")
    lines.append("| rank | finding | code | models | occurrences | unrepaired |")
    lines.append("|---|---|---|---|---|---|")
    for i, f in enumerate(findings, 1):
        lines.append(
            f"| {i} | {finding_headline(f)} | {f.get('code') or '—'} | "
            f"**{f['model_count']}** | {f['occurrences']} | {f['unrepaired']} |"
        )
    lines.append("")

    lines.append("## Finding detail")
    lines.append("")
    for i, f in enumerate(findings, 1):
        lines.append(f"### {i}. {finding_headline(f)}")
        lines.append("")
        if f.get("code"):
            gloss = glossary.get(f["code"])
            lines.append(f"- **Diagnostic code:** `{f['code']}`" + (f" — *{gloss}*" if gloss else ""))
        lines.append(f"- **Distinct models:** {f['model_count']} — {', '.join(f['models'])}")
        lines.append(f"- **Occurrences:** {f['occurrences']} (unrepaired: {f['unrepaired']})")
        if f.get("messages"):
            lines.append(f"- **Compiler said:** {f['messages'][0]}")
        lines.append(f"- **Action:** {suggested_action(f)}")
        ex = f["examples"][0] if f["examples"] else None
        if ex:
            lines.append("")
            lines.append(f"  Example (model `{ex['model']}`, intent: *{ex['intent'] or 'unspecified'}*):")
            lines.append("")
            lines.append("  ```")
            for ln in (ex.get("source_excerpt") or "").splitlines():
                lines.append("  " + ln)
            lines.append("  ```")
        lines.append("")
    return "\n".join(lines) + "\n"


def pct(n: int, d: int) -> str:
    return f"{(100.0 * n / d):.0f}%" if d else "0%"


def render_ideas_append(report: dict[str, Any], glossary: dict[str, str], min_models: int) -> str:
    """A clearly-marked, auto-generated block for the standing backlog. Only
    findings broad enough to be actionable (>= min_models distinct models, or an
    unrepaired missing-construct) are included."""
    actionable = [
        f for f in report["findings"]
        if f["model_count"] >= min_models
        or (f["kind"] == "missing_construct" and f["unrepaired"] > 0)
    ]
    s = report["summary"]
    out: list[str] = []
    out.append("")
    out.append("---")
    out.append("")
    out.append(f"## Mined from generative idea-mining — {report['generated_at'][:10]}")
    out.append("")
    out.append(
        f"*Auto-generated by `tools/aether_idea_miner.py` (the no-oracle, free-form sibling of "
        f"`aether_doc_bench.py`): {s['model_count']} models freely wrote {s['programs_total']} "
        f"Aether programs against `aether` {report['aether_version']}; "
        f"{s['programs_success']} compiled+ran. Findings below are where models reached for "
        f"something missing or tripped on an existing rule, ranked by distinct-model breadth. "
        f"Curate into the sections above as they are triaged.*"
    )
    out.append("")
    if not actionable:
        out.append("_No findings met the breadth threshold this run._")
        out.append("")
        return "\n".join(out)

    for f in actionable:
        status = "gap" if f["kind"] == "silent" else "idea"
        head = finding_headline(f)
        out.append(f"### {head} — {f['model_count']} model(s) — *{status}*")
        bits = []
        if f.get("code"):
            gloss = glossary.get(f["code"])
            bits.append(f"`{f['code']}`" + (f" ({gloss})" if gloss else ""))
        bits.append(f"hit by {f['model_count']} distinct model(s), {f['occurrences']} occurrence(s), "
                    f"{f['unrepaired']} not rescued by repair")
        out.append(" · ".join(bits) + ".")
        if f.get("messages"):
            out.append("")
            out.append(f"Compiler diagnostic: `{f['messages'][0]}`")
        ex = f["examples"][0] if f["examples"] else None
        if ex and ex.get("source_excerpt"):
            out.append("")
            out.append(f"Minimal example (model `{ex['model']}`, intent: {ex['intent'] or 'unspecified'}):")
            out.append("")
            out.append("```")
            out.extend(ex["source_excerpt"].splitlines())
            out.append("```")
        out.append("")
        out.append(f"**Suggested action:** {suggested_action(f)}")
        out.append("")
        out.append(f"Models: {', '.join(f['models'])}.")
        out.append("")
    return "\n".join(out)


# --------------------------------------------------------------------------- #
# Orchestration
# --------------------------------------------------------------------------- #
def load_destination_guide_hints(config_path: pathlib.Path) -> dict[str, str]:
    """Pick up an optional per-destination ``"guide"`` field from the raw config
    (the Destination dataclass doesn't carry it)."""
    hints: dict[str, str] = {}
    try:
        raw = json.loads(config_path.read_text(encoding="utf-8"))
    except Exception:
        return hints
    for item in raw.get("destinations", []):
        if isinstance(item, dict) and item.get("guide") in ("full", "small"):
            hints[item["id"]] = item["guide"]
    return hints


def model_stats(programs: list[dict[str, Any]]) -> dict[str, int]:
    success = sum(1 for p in programs if p.get("success"))
    fixed = sum(1 for p in programs if p.get("fixed_by_repair"))
    return {
        "programs": len(programs),
        "success": success,
        "fixed": fixed,
        "failed": len(programs) - success,
    }


def system_key(destination: adb.Destination, overrides: dict[str, str] | None = None) -> str:
    """A stable identifier for the *system* a destination runs on, so models on
    the same host stay sequential (one Ollama / LM Studio serves one model at a
    time and would thrash on concurrent requests) while different hosts run in
    parallel. Uses the base_url host:port; command destinations get their own.

    An explicit per-destination ``"system"`` field in the config overrides the
    derived value. Use it to split cloud channels that share a host but tolerate
    concurrent requests (e.g. two Gemini models on one endpoint) onto separate
    systems so they run in parallel, or to pin two hosts together as one."""
    if overrides:
        ov = overrides.get(destination.destination_id)
        if ov:
            return ov
    # T'ra scheduler destinations are serialized PER ENDPOINT: the scheduler's
    # per-target concurrency is not reliably enforced, so the miner keeps at most
    # one job in flight PER target endpoint (destinations sharing a preferred
    # target share a lane), while DIFFERENT endpoints run concurrently. Group by
    # the preferred target (fall back to model/id if none is set).
    if destination.kind == "tra_scheduler":
        eb = destination.extra_body or {}
        targets = eb.get("preferred_targets") or ([eb["target"]] if eb.get("target") else [])
        endpoint = targets[0] if targets else (destination.model or destination.destination_id)
        return f"tra:{endpoint}"
    if destination.base_url:
        netloc = urlparse(destination.base_url).netloc
        return netloc or destination.base_url
    return f"cmd:{destination.destination_id}"


def group_destinations_by_system(
    destinations: list[adb.Destination],
    overrides: dict[str, str] | None = None,
) -> list[list[adb.Destination]]:
    """Partition destinations into per-system groups, preserving the original
    order both of the groups (by first appearance) and within each group."""
    groups: dict[str, list[adb.Destination]] = {}
    for d in destinations:
        groups.setdefault(system_key(d, overrides), []).append(d)
    return list(groups.values())


def load_destination_system_overrides(config_path: pathlib.Path) -> dict[str, str]:
    """Pick up an optional per-destination ``"system"`` field from the raw config
    (the Destination dataclass doesn't carry it)."""
    out: dict[str, str] = {}
    try:
        raw = json.loads(config_path.read_text(encoding="utf-8"))
    except Exception:
        return out
    for item in raw.get("destinations", []):
        if isinstance(item, dict) and item.get("system"):
            out[item["id"]] = str(item["system"])
    return out


def run_miner(args: argparse.Namespace) -> dict[str, Any]:
    destinations = adb.load_destinations(args.destinations_config)
    if args.destination:
        wanted = set(args.destination)
        destinations = [d for d in destinations if d.destination_id in wanted]
    if not destinations:
        raise SystemExit("no destinations selected")

    guide_hints = load_destination_guide_hints(args.destinations_config)
    system_overrides = load_destination_system_overrides(args.destinations_config)
    aether_version, aether_version_raw = adb.capture_aether_version(args.aether_bin)

    # Resume support: carry over only *completed* destinations from an existing
    # report. A model gets its "stats" field set when its loop finishes (even on
    # a caught error), so "stats present" marks it done. A destination that was
    # interrupted mid-run is treated as not-done and re-run fresh (its partial
    # record is discarded), so resume never silently skips incomplete work.
    existing_models: dict[str, dict[str, Any]] = {}
    if args.resume and args.output_json and args.output_json.exists():
        try:
            prior = json.loads(args.output_json.read_text(encoding="utf-8"))
            for mr in prior.get("models", []):
                if "stats" in mr:
                    existing_models[mr["destination_id"]] = mr
        except Exception:
            existing_models = {}

    report: dict[str, Any] = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "aether_bin": str(args.aether_bin),
        "aether_version": aether_version,
        "aether_version_raw": aether_version_raw,
        "destinations_config": str(args.destinations_config),
        # The full planned destination list (in run order) so a dashboard can
        # show models that have not started yet, not just the ones already in
        # report["models"].
        "planned_destinations": [d.destination_id for d in destinations],
        "destination_count": len(destinations),
        "programs_per_model": args.programs_per_model,
        "rounds": args.rounds,
        "repair_attempts": args.repair_attempts,
        "temperature": args.temperature,
        "models": [],
        "findings": [],
        "summary": {},
    }

    # One lock guards every mutation of the shared report (model append, program
    # append, stats, and the persist read-recompute-write). The slow work
    # (generation, compile, repair) happens OUTSIDE the lock, so system groups
    # run truly concurrently and only serialize on the brief checkpoint.
    lock = threading.Lock()

    def persist_locked() -> None:
        if args.output_json:
            report["summary"] = build_summary(report["models"])
            report["findings"] = mine_findings(report["models"])
            adb.write_json_atomic(args.output_json, report)

    def log(msg: str) -> None:
        if args.progress:
            print(msg, file=sys.stderr, flush=True)

    def process_destination(destination: adb.Destination) -> None:
        did = destination.destination_id
        if did in existing_models:
            with lock:
                report["models"].append(existing_models[did])
                persist_locked()
            log(f"[skip] {did} (resumed from existing report)")
            return

        if args.temperature is not None:
            destination.temperature = args.temperature

        guide_name = choose_guide_name(destination, args.guide, guide_hints.get(did))
        guide_text = adb.read_text(guide_path_for(guide_name))

        mr: dict[str, Any] = {
            "destination_id": did,
            "model": destination.model,
            "type": destination.kind,
            "guide": guide_name,
            "system": system_key(destination, system_overrides),
            "programs": [],
        }
        with lock:
            report["models"].append(mr)
            persist_locked()
        log(f"[model] {did} ({destination.model}) guide={guide_name} system={mr['system']}")

        seen_intents: list[str] = []
        try:
            for round_idx in range(args.rounds):
                prompt = build_generation_prompt(
                    guide_name=guide_name,
                    guide_text=guide_text,
                    n_programs=args.programs_per_model,
                    avoid_intents=seen_intents[-12:],
                )
                generation = generate(prompt, destination)
                raw = generation.get("raw_text", "")
                programs = extract_programs(raw)
                with lock:
                    mr.setdefault("generation_usage", []).append(adb.normalize_usage(generation.get("usage")))
                log(f"  [{did}] round {round_idx + 1}: extracted {len(programs)} program(s)")
                if not programs:
                    with lock:
                        mr.setdefault("notes", []).append(f"round {round_idx + 1}: no programs extracted from reply")
                        mr.setdefault("empty_replies", []).append(raw[:1500])
                        persist_locked()
                for prog in programs:
                    rec = process_program(prog, destination, guide_name, guide_text, args)
                    rec["round"] = round_idx + 1
                    with lock:
                        mr["programs"].append(rec)
                        persist_locked()
                    if rec.get("intent"):
                        seen_intents.append(rec["intent"])
                    if args.progress:
                        flag = "ok " if rec["success"] else "FAIL"
                        ff = rec.get("initial_failure") or {}
                        tag = ff.get("code") or ("runtime" if ff.get("phase") == "runtime"
                                                 else ("silent" if ff.get("silent") else ""))
                        log(f"    [{did}] {flag} rc={rec['final_returncode']} "
                            f"{'fixed ' if rec['fixed_by_repair'] else ''}{tag} "
                            f":: {(rec.get('intent') or '')[:60]}")
        except Exception as exc:  # unreachable endpoint, etc. — record and move on
            with lock:
                mr["error"] = str(exc)
            log(f"  [error] {did}: {exc}")

        with lock:
            mr["stats"] = model_stats(mr["programs"])
            persist_locked()

    # Models on the same system run sequentially; different systems run in
    # parallel (it is fine to hit different hosts at once).
    groups = group_destinations_by_system(destinations, system_overrides)
    if args.sequential or len(groups) <= 1:
        for destination in destinations:
            process_destination(destination)
    else:
        import concurrent.futures as _cf
        workers = min(len(groups), max(1, args.max_parallel_systems))
        log(f"[parallel] {len(groups)} systems, up to {workers} concurrent "
            f"({', '.join(sorted({system_key(d, system_overrides) for d in destinations}))})")

        def run_group(group: list[adb.Destination]) -> None:
            for destination in group:
                process_destination(destination)

        with _cf.ThreadPoolExecutor(max_workers=workers) as ex:
            futures = [ex.submit(run_group, g) for g in groups]
            for fut in _cf.as_completed(futures):
                fut.result()

    with lock:
        report["summary"] = build_summary(report["models"])
        report["findings"] = mine_findings(report["models"])
        if args.output_json:
            adb.write_json_atomic(args.output_json, report)
    return report


def build_summary(model_records: list[dict[str, Any]]) -> dict[str, Any]:
    programs_total = sum(len(mr.get("programs", [])) for mr in model_records)
    programs_success = sum(1 for mr in model_records for p in mr.get("programs", []) if p.get("success"))
    fixed = sum(1 for mr in model_records for p in mr.get("programs", []) if p.get("fixed_by_repair"))
    models_ok = sum(1 for mr in model_records if mr.get("programs"))
    findings = mine_findings(model_records)
    return {
        "model_count": len(model_records),
        "models_ok": models_ok,
        "programs_total": programs_total,
        "programs_success": programs_success,
        "programs_failed": programs_total - programs_success,
        "fixed_by_repair": fixed,
        "finding_count": len(findings),
    }


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--destinations-config", type=pathlib.Path, default=DEFAULT_DESTINATIONS,
                   help="destination profile JSON (default: destinations.m5t.json)")
    p.add_argument("--destination", action="append", default=[],
                   help="restrict to one or more destination ids; repeatable")
    p.add_argument("--list-destinations", action="store_true", help="list configured destinations and exit")
    p.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN,
                   help="path to the gating aether binary (resolved to absolute)")
    p.add_argument("--guide", choices=("auto", "full", "small"), default="auto",
                   help="which guide to inject (default: auto by context/model size)")
    p.add_argument("--programs-per-model", type=int, default=5,
                   help="programs requested per generation call (default: 5)")
    p.add_argument("--rounds", type=int, default=1,
                   help="generation calls per model; later rounds nudge away from prior intents (default: 1)")
    p.add_argument("--repair-attempts", type=int, default=1,
                   help="repair passes on a failing program, fed the coded diagnostic (default: 1)")
    p.add_argument("--repair-feedback-limit", type=int, default=1200,
                   help="max chars of source/stdout/stderr in a repair prompt section")
    p.add_argument("--timeout-seconds", type=int, default=20, help="per-program compile+run timeout")
    p.add_argument(
        "--sandbox-deny",
        default="net,proc",
        help="VM 2.0 Phase 6 --deny classes applied to every generated program run; this harness is "
        "the no-oracle, generative, model-code-execution case, so denying net,proc by default is the "
        "conservative choice. Pass an empty string to disable.",
    )
    p.add_argument("--max-parallel-systems", type=int, default=8,
                   help="run up to N systems (distinct base_url hosts) concurrently; "
                        "models on the same host stay sequential (default: 8)")
    p.add_argument("--sequential", action="store_true",
                   help="disable cross-system parallelism; run every destination one at a time")
    p.add_argument("--temperature", type=float, default=0.7,
                   help="generation temperature override for variety (default: 0.7; <0 uses provider default)")
    p.add_argument("--output-json", type=pathlib.Path, default=None,
                   help="write the full JSON report here (checkpointed after every program)")
    p.add_argument("--output-md", type=pathlib.Path, default=None, help="write the ranked Markdown report here")
    p.add_argument("--resume", action="store_true",
                   help="skip destinations already present in --output-json")
    p.add_argument("--append-ideas", action="store_true",
                   help="append actionable findings to the standing ideas backlog")
    p.add_argument("--ideas-file", type=pathlib.Path, default=DEFAULT_IDEAS_FILE,
                   help="backlog file for --append-ideas")
    p.add_argument("--ideas-min-models", type=int, default=2,
                   help="min distinct models for a finding to be appended (default: 2)")
    p.add_argument("--progress", action="store_true", help="print per-program progress to stderr")
    return p


def main() -> int:
    args = build_arg_parser().parse_args()
    args.aether_bin = args.aether_bin.resolve()

    if args.list_destinations:
        for d in adb.load_destinations(args.destinations_config):
            print(f"{d.destination_id}\t{d.kind}\t{d.model or '-'}")
        return 0

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    report = run_miner(args)

    # Glossary from the full guide (superset of codes) for code -> rule mapping.
    glossary = load_code_glossary(FULL_GUIDE)

    md = render_markdown(report, glossary)
    if args.output_md:
        args.output_md.parent.mkdir(parents=True, exist_ok=True)
        args.output_md.write_text(md, encoding="utf-8")
    else:
        print(md)

    if args.append_ideas:
        block = render_ideas_append(report, glossary, args.ideas_min_models)
        with args.ideas_file.open("a", encoding="utf-8") as fh:
            fh.write(block)
        print(f"[append-ideas] appended findings to {args.ideas_file}", file=sys.stderr, flush=True)

    s = report["summary"]
    print(f"\n[done] {s['model_count']} models, {s['programs_total']} programs, "
          f"{s['programs_success']} ran clean, {s['finding_count']} findings",
          file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
