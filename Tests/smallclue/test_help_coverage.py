#!/usr/bin/env python3
"""Verify every registered smallclue applet has a help-table usage entry."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CORE_C = ROOT / "src" / "smallclue" / "src" / "core.c"

APPLET_TABLE_RE = re.compile(
    r"static const SmallclueApplet kSmallclueApplets\[\] = \{(.*?)\n\};\n\n"
    r"static const SmallclueAppletHelp kSmallclueAppletHelp\[\] = \{",
    re.S,
)
HELP_TABLE_RE = re.compile(
    r"static const SmallclueAppletHelp kSmallclueAppletHelp\[\] = \{(.*?)\n\};\n\n"
    r"static size_t kSmallclueAppletCount",
    re.S,
)
ENTRY_NAME_RE = re.compile(r'\{"([^\"]+)",')


def _extract_names(match_text: str) -> list[str]:
    return [name for name in ENTRY_NAME_RE.findall(match_text) if name != "NULL"]


def main() -> int:
    text = CORE_C.read_text(encoding="utf-8")

    applet_match = APPLET_TABLE_RE.search(text)
    help_match = HELP_TABLE_RE.search(text)
    if not applet_match or not help_match:
        print("FAIL: unable to locate applet/help tables in core.c", file=sys.stderr)
        return 2

    applets = _extract_names(applet_match.group(1))
    helps = _extract_names(help_match.group(1))

    missing = [name for name in applets if name not in helps]
    extras = [name for name in helps if name not in applets]

    if missing:
        print("FAIL: missing help entries for applets:")
        for name in missing:
            print(f"  - {name}")
    if extras:
        print("FAIL: help entries without applet registration:")
        for name in extras:
            print(f"  - {name}")

    if missing or extras:
        return 1

    print(f"PASS: {len(applets)} applet entries all covered by help table")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
