#!/usr/bin/env python3
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"
EXAMPLE_PATTERN = re.compile(r"(Examples/[A-Za-z0-9_./-]+)")


def main() -> int:
    failures: list[tuple[Path, str]] = []
    checked = 0

    for workflow in sorted(WORKFLOW_DIR.glob("*.yml")):
        text = workflow.read_text(encoding="utf-8", errors="ignore")
        for match in EXAMPLE_PATTERN.finditer(text):
            rel = match.group(1).rstrip(".,:;)'\"")
            candidate = (REPO_ROOT / rel).resolve()
            checked += 1
            if not candidate.exists():
                failures.append((workflow.relative_to(REPO_ROOT), rel))

    if failures:
        print("FAIL: workflow example path validation failed")
        for workflow, rel in failures:
            print(f"  {workflow}: missing path '{rel}'")
        return 1

    print(f"PASS: validated {checked} workflow Example path reference(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
