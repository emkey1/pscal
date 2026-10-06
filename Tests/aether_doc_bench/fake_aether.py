#!/usr/bin/env python3
"""A stand-in for the `aether` binary, for the harness's offline tests.

It lets test_doc_bench_offline.py drive tools/aether_doc_bench.py end to end
(snapshot, skew guard, seeds, timeouts, infra handling) on a machine with no
compiler, such as the python-only CI job. It does not run Aether: a "program"
is a list of directives, one per line, and every other line is ignored.

  //! print TEXT       write TEXT and a newline to stdout
  //! write TEXT       write TEXT to stdout, no newline
  //! stderr TEXT      write TEXT and a newline to stderr
  //! echo-stdin       copy stdin to stdout
  //! count-stdin      print "lines=N" for the number of stdin lines
  //! sleep SECONDS    sleep (float)
  //! loop             spin forever (the harness must time it out)
  //! flood N          write N identical warning lines to stderr
  //! diag JSON        with --diagnostics-json, write JSON to stderr
  //! exit N           exit status N (default 0)

`--version` prints "Aether Compiler Version: $FAKE_AETHER_VERSION" (default
2026-10-06-1+fake000). With $FAKE_AETHER_LOG set, every call appends one JSON
line {"argv": [...]} to that file, so a test can see exactly which binary and
flags the harness used.
"""

from __future__ import annotations

import json
import os
import pathlib
import sys
import time


def main(argv: list[str]) -> int:
    log = os.environ.get("FAKE_AETHER_LOG")
    if log:
        with open(log, "a", encoding="utf-8") as handle:
            handle.write(json.dumps({"argv": [sys.argv[0], *argv]}) + "\n")

    if "--version" in argv or "-v" in argv:
        version = os.environ.get("FAKE_AETHER_VERSION", "2026-10-06-1+fake000")
        print(f"Aether Compiler Version: {version}")
        return 0

    diagnostics_json = "--diagnostics-json" in argv
    program = None
    skip_next = False
    for arg in argv:
        if skip_next:
            skip_next = False
            continue
        if arg == "--deny":
            skip_next = True
            continue
        if arg.startswith("-"):
            continue
        program = arg
        break
    if program is None:
        print("fake_aether: no program given", file=sys.stderr)
        return 2

    status = 0
    directives = 0
    for line in pathlib.Path(program).read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped.startswith("//!"):
            continue
        directives += 1
        verb, _, rest = stripped[3:].strip().partition(" ")
        if verb == "print":
            sys.stdout.write(rest + "\n")
        elif verb == "write":
            sys.stdout.write(rest)
        elif verb == "stderr":
            sys.stderr.write(rest + "\n")
        elif verb == "echo-stdin":
            sys.stdout.write(sys.stdin.read())
        elif verb == "count-stdin":
            sys.stdout.write(f"lines={len(sys.stdin.read().splitlines())}\n")
        elif verb == "sleep":
            sys.stdout.flush()
            time.sleep(float(rest or "0"))
        elif verb == "loop":
            sys.stdout.flush()
            while True:
                time.sleep(0.05)
        elif verb == "flood":
            for _ in range(int(rest or "1")):
                sys.stderr.write("prog.aether:1: warning: [NARROW-001] narrowing assignment\n")
        elif verb == "diag":
            if diagnostics_json:
                sys.stderr.write(rest + "\n")
        elif verb == "exit":
            status = int(rest or "0")
        sys.stdout.flush()
    if directives == 0:
        print(f"{pathlib.Path(program).name}:1: [SYN-001] fake_aether: no //! directives", file=sys.stderr)
        return 1
    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
