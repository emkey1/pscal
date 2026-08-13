#!/usr/bin/env python3
"""Deterministic fake model that fails once, then repairs successfully."""

from __future__ import annotations

import pathlib
import re
import sys


def extract_task_id(prompt: str) -> str:
    match = re.search(r"^\s*Task ID:\s*(\S+)\s*$", prompt, re.MULTILINE)
    if not match:
        raise SystemExit("unable to locate Task ID in prompt")
    return match.group(1)


BROKEN = {
    "hello_fx": """\
fn main() -> Void {
    println("hello from benchmark");
    ret;
}
""",
}


FIXED = {
    "hello_fx": """\
fn main() -> Void {
    fx {
        println("hello from benchmark");
    }
    ret;
}
""",
}


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: mock_repair_model.py PROMPT_FILE")
    prompt = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    task_id = extract_task_id(prompt)
    if "repair attempt number:" in prompt.lower():
        sys.stdout.write(FIXED[task_id])
    else:
        sys.stdout.write(BROKEN[task_id])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
