#!/usr/bin/env python3
"""Self-contained harness for Rea scope conformance tests.

This script reads `tests/manifest.json`, materialises each test case (with
optional seeded randomisation), writes runnable snippets, executes the configured
Rea compiler/runtime command, compares the observed results against expectations,
and produces human-readable plus machine-readable summaries.

Usage examples:
    python3 rea_scope_test_harness.py --list
    python3 rea_scope_test_harness.py --only shadow --seed 2025
    python3 rea_scope_test_harness.py --cmd "../../build/bin/rea --strict {source}"
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import sys
import textwrap
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Constants & simple utilities
# ---------------------------------------------------------------------------

HARNESS_ROOT = Path(__file__).resolve().parent


def locate_repo_root(start: Path) -> Path:
    for candidate in [start] + list(start.parents):
        if (candidate / 'CMakeLists.txt').exists() or (candidate / '.git').exists():
            return candidate
    return start


REPO_ROOT = locate_repo_root(HARNESS_ROOT)
DEFAULT_MANIFEST = HARNESS_ROOT / 'tests' / 'manifest.json'
DEFAULT_OUT_DIR = HARNESS_ROOT / "out"
DEFAULT_TIMEOUT = 20.0
PLACEHOLDER_PATTERN = re.compile(r"\{\{([a-zA-Z0-9_\-\.]+)\}\}")
VALID_EXPECTATIONS = {"compile_ok", "compile_error", "runtime_ok", "runtime_error"}
CSV_HEADER = ["name", "category", "expect", "exit_code", "pass", "reason"]


def derive_default_command() -> str:
    exe = (REPO_ROOT / "build" / "bin" / "rea").resolve()
    return f"{shlex.quote(str(exe))} {{source}}"


class HarnessError(RuntimeError):
    """Raised for unrecoverable configuration issues."""


@dataclass
class ResolvedFile:
    """Represents an auxiliary file generated for a test."""

    path: Path
    contents: str


@dataclass
class ResolvedTest:
    """Materialised test case ready to execute."""

    raw: Dict[str, Any]
    test_id: str
    name: str
    category: str
    description: str
    expect: str
    code: str
    expected_stdout: Optional[str]
    expected_stderr_substring: Optional[str]
    main_path: Path
    support_files: List[ResolvedFile] = field(default_factory=list)
    reason: Optional[str] = None
    seed: int = 0

    def short_status(self) -> str:
        return f"{self.test_id} ({self.category})"


# ---------------------------------------------------------------------------
# Placeholder rendering
# ---------------------------------------------------------------------------


def _hash_to_int(seed: int, text: str) -> int:
    h = hashlib.sha256(f"{seed}:{text}".encode("utf-8")).digest()
    return int.from_bytes(h[:8], "big")


def render_template(template: str, context: Dict[str, Any]) -> str:
    """Replace {{placeholders}} in template using the provided context."""

    def repl(match: re.Match[str]) -> str:
        key = match.group(1)
        if key not in context:
            raise HarnessError(f"Missing placeholder '{key}' while rendering template")
        value = context[key]
        return str(value)

    return PLACEHOLDER_PATTERN.sub(repl, template)


def ensure_unique_identifier(candidate: str, context: Dict[str, Any]) -> str:
    """Avoid collisions between automatically generated identifiers."""
    used = {str(v) for k, v in context.items() if k.startswith("ident_")}
    original = candidate
    suffix = 1
    while candidate in used:
        candidate = f"{original}_{suffix}"
        suffix += 1
    return candidate


IDENT_START = "abcdefghijklmnopqrstuvwxyz"
IDENT_BODY = IDENT_START + "0123456789_"


def generate_identifier(spec: Dict[str, Any], rng: random.Random, context: Dict[str, Any]) -> str:
    min_length = spec.get("min_length", 4)
    max_length = spec.get("max_length", 10)
    if min_length > max_length:
        min_length, max_length = max_length, min_length
    length = rng.randint(min_length, max_length)
    prefix = spec.get("prefix", "")
    body = prefix
    body += rng.choice(IDENT_START)
    while len(body) < len(prefix) + length:
        body += rng.choice(IDENT_BODY)
    style = spec.get("style", "lower")
    if style == "camel":
        body = body[0] + body[1:].title()
    elif style == "upper":
        body = body.upper()
    avoid = set(spec.get("avoid_values", []))
    avoid |= {str(context.get(name)) for name in spec.get("avoid_placeholders", []) if name in context}
    candidate = body
    while candidate in avoid:
        candidate = body + rng.choice("xyz")
    if spec.get("ensure_unique", True):
        candidate = ensure_unique_identifier(candidate, context)
    return candidate


def generate_whitespace(spec: Dict[str, Any], rng: random.Random) -> str:
    min_spaces = spec.get("min", 0)
    max_spaces = spec.get("max", min_spaces)
    if max_spaces < min_spaces:
        max_spaces = min_spaces
    count = rng.randint(min_spaces, max_spaces)
    if count == 0:
        return ""
    choices = spec.get("charset", [" ", "\t"])
    return "".join(rng.choice(choices) for _ in range(count))


def generate_int(spec: Dict[str, Any], rng: random.Random) -> int:
    if "choices" in spec:
        return rng.choice(spec["choices"])
    low = spec.get("min", 0)
    high = spec.get("max", low)
    if high < low:
        low, high = high, low
    step = max(1, spec.get("step", 1))
    span = ((high - low) // step) + 1
    idx = rng.randrange(span)
    return low + idx * step


def generate_literal(spec: Dict[str, Any], rng: random.Random) -> str:
    if "choices" in spec:
        return str(rng.choice(spec["choices"]))
    return str(spec.get("value", ""))


def generate_comment(spec: Dict[str, Any], rng: random.Random) -> str:
    prefix = spec.get("prefix", "//")
    words = spec.get("words", ["TODO", "note", "scope-check"])
    count = spec.get("word_count", 2)
    chosen = [rng.choice(words) for _ in range(max(1, count))]
    return f"{prefix} {' '.join(chosen)}"


def generate_nested_block(spec: Dict[str, Any], rng: random.Random, context: Dict[str, Any]) -> Tuple[str, int]:
    indent = spec.get("indent", "    ")
    header = spec.get("header", "if (true)")
    min_depth = spec.get("min_depth", 1)
    max_depth = spec.get("max_depth", min_depth)
    if max_depth < min_depth:
        min_depth, max_depth = max_depth, min_depth
    depth = rng.randint(min_depth, max_depth)
    body_lines = spec.get("body", [])
    body_context_updates = {}
    body_placeholders = spec.get("placeholders", {})
    body_context = dict(context)
    body_context.update(body_context_updates)
    # Generate additional placeholders specific to this nested block
    for key, subspec in body_placeholders.items():
        body_context[key] = generate_value(subspec, rng, body_context)
    rendered_body_lines = []
    for raw_line in body_lines:
        rendered = render_template(raw_line, body_context)
        rendered_body_lines.append(rendered)
    lines: List[str] = []
    current_indent = indent
    for level in range(depth):
        ws = spec.get("leading_whitespace", "")
        lines.append(f"{ws}{current_indent}{header} {{")
        current_indent += indent
    for line in rendered_body_lines:
        lines.append(f"{current_indent}{line}")
    for level in range(depth, 0, -1):
        current_indent = indent * level
        lines.append(f"{current_indent}}}")
    trailing = spec.get("trailing", None)
    if trailing:
        lines.append(render_template(trailing, body_context))
    return "\n".join(lines), depth


def generate_value(spec: Dict[str, Any], rng: random.Random, context: Dict[str, Any]) -> Any:
    if isinstance(spec, str):
        return spec
    if not isinstance(spec, dict):
        return spec
    kind = spec.get("type", "literal")
    if kind == "identifier":
        return generate_identifier(spec, rng, context)
    if kind == "whitespace":
        return generate_whitespace(spec, rng)
    if kind == "int":
        return generate_int(spec, rng)
    if kind == "literal":
        return generate_literal(spec, rng)
    if kind == "comment":
        return generate_comment(spec, rng)
    if kind == "nested_block":
        block, depth = generate_nested_block(spec, rng, context)
        store_depth_as = spec.get("store_depth_as")
        if store_depth_as:
            context[store_depth_as] = depth
        return block
    if kind == "choices":
        return rng.choice(spec.get("values", [""]))
    raise HarnessError(f"Unsupported placeholder type '{kind}'")


def populate_placeholders(placeholders: Dict[str, Any], rng: random.Random, base_context: Dict[str, Any]) -> Dict[str, Any]:
    context = dict(base_context)
    for name, spec in placeholders.items():
        if name in context:
            continue
        context[name] = generate_value(spec, rng, context)
    return context


# ---------------------------------------------------------------------------
# Manifest loading and test materialisation
# ---------------------------------------------------------------------------


def sanitise_identifier(value: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_\-]+", "_", value)
    return safe.strip("_") or "test"


def normalize_output(text: str) -> str:
    lines = text.replace("\r", "").splitlines()
    stripped = [line.rstrip() for line in lines]
    return "\n".join(stripped)


def ensure_directory(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def load_manifest(path: Path) -> Dict[str, Any]:
    if not path.exists():
        raise HarnessError(f"Manifest not found at {path}")
    with path.open("r", encoding="utf-8") as fh:
        try:
            data = json.load(fh)
        except json.JSONDecodeError as exc:
            raise HarnessError(f"Failed to parse manifest: {exc}") from exc
    if "tests" not in data or not isinstance(data["tests"], list):
        raise HarnessError("Manifest is missing a 'tests' array")
    return data


def materialise_test(
    entry: Dict[str, Any],
    args: argparse.Namespace,
    base_out: Path,
    manifest_dir: Path,
    update_materialised: bool,
) -> ResolvedTest:
    missing = [key for key in ("id", "name", "category", "code", "expect") if key not in entry]
    if missing:
        raise HarnessError(f"Test entry missing fields {missing}: {entry}")
    test_id = entry["id"]
    expect = entry["expect"]
    if expect not in VALID_EXPECTATIONS:
        raise HarnessError(f"Test {test_id} has unsupported expect '{expect}'")
    base_seed = getattr(args, "seed", 1337)
    rng = random.Random(_hash_to_int(base_seed, test_id))
    context: Dict[str, Any] = {
        "test_id": test_id,
        "category": entry.get("category", "uncategorised"),
    }
    safe_id = sanitise_identifier(test_id)
    context["safe_id"] = safe_id
    context["support_dir"] = f"{safe_id}__support"
    placeholders = entry.get("placeholders", {})
    context = populate_placeholders(placeholders, rng, context)
    code_template = entry.get("code", "")
    code = render_template(code_template, context)
    ext = entry.get("extension", args.ext or "rea")
    ensure_directory(base_out)
    main_path = base_out / f"{safe_id}.{ext}"
    expected_stdout = entry.get("expected_stdout")
    expected_stderr = entry.get("expected_stderr_substring")
    if expected_stdout is not None:
        expected_stdout = render_template(expected_stdout, context)
    if expected_stderr is not None:
        expected_stderr = render_template(expected_stderr, context)
    support_files: List[ResolvedFile] = []
    for extra in entry.get("files", []):
        if "path" not in extra or "code" not in extra:
            raise HarnessError(f"Test {test_id} has malformed extra file: {extra}")
        extra_context = populate_placeholders(extra.get("placeholders", {}), rng, dict(context))
        subcode = render_template(extra["code"], extra_context)
        rel_path = Path(extra["path"])
        support_files.append(ResolvedFile(rel_path, subcode))
    reason = entry.get("failure_reason")
    resolved = ResolvedTest(
        raw=entry,
        test_id=test_id,
        name=entry.get("name", test_id),
        category=entry.get("category", "uncategorised"),
        description=entry.get("description", ""),
        expect=expect,
        code=code,
        expected_stdout=expected_stdout,
        expected_stderr_substring=expected_stderr,
        main_path=main_path,
        support_files=support_files,
        reason=reason,
        seed=base_seed,
    )
    # Materialise files to disk
    main_path.write_text(code, encoding="utf-8")
    for file in support_files:
        support_root = base_out / f"{safe_id}__support"
        target_path = support_root / file.path
        ensure_directory(target_path.parent)
        target_path.write_text(file.contents, encoding="utf-8")
    if update_materialised:
        category_dir = manifest_dir / entry.get("category", "uncategorised")
        ensure_directory(category_dir)
        materialised_path = category_dir / f"{safe_id}.{ext}"
        materialised_path.write_text(code, encoding="utf-8")
        for file in support_files:
            aux_path = category_dir / file.path
            ensure_directory(aux_path.parent)
            aux_path.write_text(file.contents, encoding="utf-8")
    return resolved


# ---------------------------------------------------------------------------
# Execution & evaluation
# ---------------------------------------------------------------------------


def build_command(command_template: str, source: Path, extra_files: List[ResolvedFile]) -> List[str]:
    command_text = command_template.format(source=str(source.resolve()))
    return shlex.split(command_text)


def evaluate_test(
    resolved: ResolvedTest,
    args: argparse.Namespace,
    command_template: str,
    out_root: Path,
) -> Tuple[bool, str, Optional[subprocess.CompletedProcess[str]]]:
    command = build_command(command_template, resolved.main_path, resolved.support_files)
    try:
        proc = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.timeout,
            cwd=resolved.main_path.parent,
            check=False,
            text=True,
        )
    except subprocess.TimeoutExpired:
        return False, f"timeout after {args.timeout}s", None
    stdout = normalize_output(proc.stdout)
    stderr = normalize_output(proc.stderr)
    exit_code = proc.returncode
    passed = False
    reason = ""
    expect = resolved.expect
    if expect == "runtime_ok" or expect == "compile_ok":
        if exit_code != 0:
            reason = f"expected exit 0 but got {exit_code}"
        else:
            if resolved.expected_stdout is not None and stdout != normalize_output(resolved.expected_stdout):
                reason = "stdout mismatch"
            elif resolved.expected_stderr_substring and resolved.expected_stderr_substring not in stderr:
                reason = "missing expected stderr snippet"
            else:
                passed = True
                reason = "ok"
    elif expect == "compile_error" or expect == "runtime_error":
        if exit_code == 0:
            reason = "expected non-zero exit code"
        else:
            snippet = resolved.expected_stderr_substring
            if snippet and snippet not in stderr:
                reason = "stderr missing expected substring"
            else:
                passed = True
                reason = "error observed as expected"
    else:
        reason = f"unknown expectation {expect}"
    if not passed:
        write_failure_artifact(resolved, stdout, stderr, exit_code, reason, out_root)
    return passed, reason, proc


def write_failure_artifact(
    resolved: ResolvedTest,
    stdout: str,
    stderr: str,
    exit_code: int,
    reason: str,
    out_root: Path,
) -> None:
    min_dir = out_root / "min"
    ensure_directory(min_dir)
    repro_path = min_dir / f"{sanitise_identifier(resolved.test_id)}.txt"
    snippet_lines = [
        f"Test: {resolved.test_id}",
        f"Category: {resolved.category}",
        f"Expectation: {resolved.expect}",
        f"Description: {resolved.description}",
        f"Reason: {reason}",
        f"Exit code: {exit_code}",
        "--- stdout ---",
        stdout or "<empty>",
        "--- stderr ---",
        stderr or "<empty>",
        "--- source ---",
        resolved.code,
    ]
    repro_path.write_text("\n".join(snippet_lines), encoding="utf-8")


# ---------------------------------------------------------------------------
# CLI helpers
# ---------------------------------------------------------------------------


def parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Rea scope verification harness")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Path to manifest.json")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help="Directory for generated artefacts")
    parser.add_argument("--cmd", type=str, default=derive_default_command(), help="Command template to execute")
    parser.add_argument("--only", action="append", default=[], help="Substring filter applied to id/name/category/description")
    parser.add_argument("--list", action="store_true", help="List matching tests without executing")
    parser.add_argument("--update", action="store_true", help="Update materialised .rea fixtures under tests/<category>/")
    parser.add_argument("--seed", type=int, default=1337, help="Seed for deterministic random variants")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="Per-test timeout in seconds")
    parser.add_argument("--ext", type=str, default="rea", help="File extension for generated sources")
    parser.add_argument("--no-run", action="store_true", help="Generate files but skip execution")
    return parser.parse_args(argv)


def matches_filter(entry: Dict[str, Any], patterns: List[str]) -> bool:
    if not patterns:
        return True
    haystack = " ".join(
        str(entry.get(key, ""))
        for key in ("id", "name", "category", "description")
    ).lower()
    return any(pattern.lower() in haystack for pattern in patterns)


def write_csv_report(out_root: Path, rows: List[List[Any]]) -> None:
    ensure_directory(out_root)
    csv_path = out_root / "report.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(CSV_HEADER)
        writer.writerows(rows)


# ---------------------------------------------------------------------------
# Main harness flow
# ---------------------------------------------------------------------------


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    manifest = load_manifest(args.manifest)
    tests = [entry for entry in manifest.get("tests", []) if matches_filter(entry, args.only)]
    if not tests:
        print("No tests matched filter", file=sys.stderr)
        return 1
    out_root = args.out_dir
    ensure_directory(out_root / "gen")
    manifest_tests_dir = HARNESS_ROOT / "tests"

    if args.list:
        for entry in tests:
            print(f"[{entry.get('category', 'uncategorised')}] {entry.get('id')} :: {entry.get('name')}")
        return 0

    rows: List[List[Any]] = []
    results: List[Tuple[ResolvedTest, bool, str, Optional[subprocess.CompletedProcess[str]]]] = []
    for entry in tests:
        resolved = materialise_test(
            entry=entry,
            args=args,
            base_out=out_root / "gen",
            manifest_dir=manifest_tests_dir,
            update_materialised=args.update,
        )
        if args.no_run:
            print(f"[SKIP] {resolved.short_status()} :: generation only")
            continue
        ok, reason, proc = evaluate_test(resolved, args, args.cmd, out_root)
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {resolved.test_id} – {resolved.name}")
        if not ok:
            print(f"    Reason: {reason}")
            if proc is not None:
                stdout = normalize_output(proc.stdout)
                stderr = normalize_output(proc.stderr)
                if stdout.strip():
                    print("    stdout:")
                    for line in stdout.strip().splitlines():
                        print(f"        {line}")
                if stderr.strip():
                    print("    stderr:")
                    for line in stderr.strip().splitlines():
                        print(f"        {line}")
        results.append((resolved, ok, reason, proc))
        rows.append([
            resolved.test_id,
            resolved.category,
            resolved.expect,
            proc.returncode if proc else "timeout",
            "yes" if ok else "no",
            reason,
        ])
    if rows:
        write_csv_report(out_root, rows)

    failures = [entry for entry in results if not entry[1]]

    print()
    print(f"Ran {len(results)} rea scope test(s); {len(failures)} failure(s)")

    return 0 if not failures else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as exc:
        print(f"Harness error: {exc}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        print("Interrupted", file=sys.stderr)
        sys.exit(130)
