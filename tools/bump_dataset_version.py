#!/usr/bin/env python3
"""Bump the date-stamp version on an Aether benchmark dataset.

Scheme: YYYY-MM-DD-N (same as the guides and the language VERSION) -- the date of
the last change to the dataset's tasks, with N resetting per day. The stamp lives
in the dataset JSON's top-level "version" field and is recorded into every result
by aether_doc_bench.py (as tasks_version), so a score ties to the exact dataset
revision.

Run this when you add, change, or remove tasks or expected outputs in a dataset,
before committing -- not on unrelated edits. The targeted edit preserves the
file's formatting (it rewrites only the "version" value).

Usage:
    python3 tools/bump_dataset_version.py Tests/aether_doc_bench/tasks_cs.json
    python3 tools/bump_dataset_version.py --show Tests/aether_doc_bench/*.json
"""
import sys
import re
import datetime

VALUE = re.compile(r'("version"\s*:\s*")([^"]*)(")')
DATE = re.compile(r"^(\d{4}-\d{2}-\d{2})-(\d+)$")


def process(path: str, show: bool) -> str:
    txt = open(path, encoding="utf-8").read()
    m = VALUE.search(txt)
    cur = m.group(2) if m else ""
    if show:
        return f"{path}: {cur or '(no version field)'}"
    if not m:
        return f'{path}: no "version" field -- add one before bumping'
    today = datetime.date.today().isoformat()
    dm = DATE.match(cur)
    newn = int(dm.group(2)) + 1 if (dm and dm.group(1) == today) else 1
    ver = f"{today}-{newn}"
    txt = txt[:m.start()] + m.group(1) + ver + m.group(3) + txt[m.end():]
    open(path, "w", encoding="utf-8").write(txt)
    return f"{path}: {cur or '(unset)'} -> {ver}"


if __name__ == "__main__":
    show = "--show" in sys.argv
    paths = [a for a in sys.argv[1:] if a != "--show"]
    if not paths:
        raise SystemExit(__doc__)
    for p in paths:
        print(process(p, show))
