#!/usr/bin/env python3
"""Runs the Phase 1e verifier's malformed-.bc corpus (Docs/pscal_vm2_plan.md
§5.5) through pscalvm and checks that every entry behaves as its manifest
expects: golden controls load and run (exit 0), corrupt entries are
rejected cleanly (nonzero exit, no crash signal -- never a segfault/abort),
with the entry's expect_stderr text in stderr when the manifest names one. A
control's expect_stdout must appear in stdout, and an entry's env is added to
pscalvm's environment.

Usage: python3 run_corpus_tests.py [--pscalvm-bin PATH] [--corpus DIR]
"""

import argparse
import json
import os
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

CRASH_SIGNALS = {4, 6, 8, 10, 11}  # SIGILL, SIGABRT, SIGFPE, SIGBUS, SIGSEGV


def crash_signal(returncode):
    """The crash signal behind a pscalvm exit status, or None. A negative
    return code means "killed by signal -N". The VM also traps SIGSEGV and
    SIGABRT to restore the terminal and then exits 128 + the signal
    (vmSignalHandler in backend_ast/builtin.c), so a crash can arrive as an
    ordinary exit status too."""
    if returncode < 0 and -returncode in CRASH_SIGNALS:
        return -returncode
    if returncode - 128 in CRASH_SIGNALS:
        return returncode - 128
    return None


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
        env = dict(os.environ)
        env.update(entry.get("env", {}))
        proc = subprocess.run([args.pscalvm_bin, path], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        sig = crash_signal(proc.returncode)
        ok = proc.returncode == 0

        if sig is not None:
            failures.append(f"{entry['file']}: CRASHED (signal {sig}) -- {entry['note']}")
            print(f"[CRASH] {entry['file']} (signal {sig})")
            continue

        if entry["expect_ok"]:
            want_out = entry.get("expect_stdout")
            stdout = proc.stdout.decode(errors="replace")
            if not ok:
                failures.append(f"{entry['file']}: expected clean success, got exit {proc.returncode} "
                                 f"-- stderr: {proc.stderr.decode(errors='replace')[:200]}")
                print(f"[FAIL] {entry['file']} (expected success, exit={proc.returncode})")
            elif want_out is not None and want_out not in stdout:
                failures.append(f"{entry['file']}: ran, but stdout lacks {want_out!r} "
                                 f"-- stdout: {stdout[:200]}")
                print(f"[FAIL] {entry['file']} (ran, but not with {want_out!r})")
            else:
                print(f"[PASS] {entry['file']} (loaded and ran)")
        else:
            stderr = proc.stderr.decode(errors="replace")
            want = entry.get("expect_stderr")
            if ok:
                failures.append(f"{entry['file']}: expected clean rejection, but it ran successfully "
                                 f"({entry['note']})")
                print(f"[FAIL] {entry['file']} (expected rejection, got exit 0)")
            elif want is not None and want not in stderr:
                failures.append(f"{entry['file']}: rejected (exit {proc.returncode}) but stderr lacks "
                                 f"{want!r} -- stderr: {stderr[:200]}")
                print(f"[FAIL] {entry['file']} (rejected, but not with {want!r})")
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
