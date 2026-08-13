#!/usr/bin/env python3
"""Runs the Phase 1e verifier's malformed-.bc corpus (Docs/pscal_vm2_plan.md
§5.5) through pscalvm and checks that every entry behaves as its manifest
expects: golden controls load and run (exit 0), corrupt entries are
rejected cleanly (nonzero exit, no crash signal -- never a segfault/abort).

Usage: python3 run_corpus_tests.py [--pscalvm-bin PATH] [--corpus DIR]
"""

import argparse
import json
import os
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Negative return codes from subprocess mean "killed by signal -N" on POSIX.
CRASH_SIGNALS = {4, 6, 8, 10, 11}  # SIGILL, SIGABRT, SIGFPE, SIGBUS, SIGSEGV


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pscalvm-bin", default=os.path.join(REPO_ROOT, "build", "bin", "pscalvm"))
    ap.add_argument("--corpus", default=os.path.join(os.path.dirname(__file__), "corpus"))
    args = ap.parse_args()

    manifest_path = os.path.join(args.corpus, "manifest.json")
    if not os.path.exists(manifest_path):
        print(f"no manifest at {manifest_path}; run generate_corpus.py first", file=sys.stderr)
        return 1
    with open(manifest_path) as f:
        manifest = json.load(f)

    if not os.path.exists(args.pscalvm_bin):
        print(f"pscalvm binary not found at {args.pscalvm_bin}", file=sys.stderr)
        return 1

    failures = []
    for entry in manifest:
        path = os.path.join(args.corpus, entry["file"])
        proc = subprocess.run([args.pscalvm_bin, path],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        crashed = proc.returncode < 0 and (-proc.returncode) in CRASH_SIGNALS
        ok = proc.returncode == 0

        if crashed:
            failures.append(f"{entry['file']}: CRASHED (signal {-proc.returncode}) -- {entry['note']}")
            print(f"[CRASH] {entry['file']} (signal {-proc.returncode})")
            continue

        if entry["expect_ok"]:
            if not ok:
                failures.append(f"{entry['file']}: expected clean success, got exit {proc.returncode} "
                                 f"-- stderr: {proc.stderr.decode(errors='replace')[:200]}")
                print(f"[FAIL] {entry['file']} (expected success, exit={proc.returncode})")
            else:
                print(f"[PASS] {entry['file']} (loaded and ran)")
        else:
            if ok:
                failures.append(f"{entry['file']}: expected clean rejection, but it ran successfully "
                                 f"({entry['note']})")
                print(f"[FAIL] {entry['file']} (expected rejection, got exit 0)")
            else:
                print(f"[PASS] {entry['file']} (rejected cleanly, exit={proc.returncode})")

    print()
    print(f"Ran {len(manifest)} corpus file(s); {len(failures)} failure(s)")
    if failures:
        print()
        print("Failures:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
