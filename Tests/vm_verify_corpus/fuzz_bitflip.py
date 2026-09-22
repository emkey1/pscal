#!/usr/bin/env python3
"""Short single-bit-flip fuzz sweep for the Phase 1e verifier
(Docs/pscal_vm2_plan.md §5.5, §8): flips every bit of a real .bc file one at
a time and runs it through pscalvm, asserting zero crashes (a clean nonzero
exit or a successful run are both fine; a crash signal is not). This is the
quick local pass; a broader/longer sweep (more seed files, multi-bit flips)
is a good claw-idle background job -- see CLAUDE.md's fleet rules.

Usage: python3 fuzz_bitflip.py [--pscalvm-bin PATH] [--seed PATH] [--limit N]
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_corpus_tests import crash_signal  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pscalvm-bin", default=os.path.join(REPO_ROOT, "build", "bin", "pscalvm"))
    ap.add_argument("--seed", default=os.path.join(os.path.dirname(__file__), "corpus", "golden_hello.bc"))
    ap.add_argument("--limit", type=int, default=0, help="Stop after N mutation points (0 = no limit)")
    args = ap.parse_args()

    if not os.path.exists(args.pscalvm_bin):
        print(f"pscalvm binary not found at {args.pscalvm_bin}", file=sys.stderr)
        return 1
    if not os.path.exists(args.seed):
        print(f"seed .bc not found at {args.seed}; run generate_corpus.py first", file=sys.stderr)
        return 1

    with open(args.seed, "rb") as f:
        seed = f.read()

    # Per-run file, so sweeps against two builds can run side by side.
    fd, tmp_path = tempfile.mkstemp(prefix="fuzz_", suffix=".bc")
    os.close(fd)
    total = 0
    crashes = []
    hangs = []
    clean_fail = 0
    clean_ok = 0

    try:
        for byte_idx in range(len(seed)):
            for bit in range(8):
                if args.limit and total >= args.limit:
                    break
                mutant = bytearray(seed)
                mutant[byte_idx] ^= (1 << bit)
                with open(tmp_path, "wb") as f:
                    f.write(mutant)
                total += 1
                try:
                    proc = subprocess.run([args.pscalvm_bin, tmp_path],
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                           timeout=5)
                except subprocess.TimeoutExpired:
                    # A mutated jump can create a tight infinite loop (e.g. a
                    # JUMP retargeted to itself) that the verifier can't and
                    # shouldn't try to detect (halting problem) -- not a
                    # crash/memory-safety issue, just noted separately.
                    hangs.append((byte_idx, bit))
                    continue
                sig = crash_signal(proc.returncode)
                if sig is not None:
                    crashes.append((byte_idx, bit, sig))
                elif proc.returncode == 0:
                    clean_ok += 1
                else:
                    clean_fail += 1
            if args.limit and total >= args.limit:
                break
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

    print(f"{total} mutation point(s): {clean_fail} clean rejection(s), "
          f"{clean_ok} ran anyway (inert flip), {len(hangs)} hang(s) (loop, not a crash), "
          f"{len(crashes)} crash(es)")
    if hangs:
        print(f"Hangs (first 20 of {len(hangs)}): {hangs[:20]}")
    if crashes:
        print("Crashes:")
        for byte_idx, bit, sig in crashes[:50]:
            print(f"  byte {byte_idx} bit {bit}: signal {sig}")
        if len(crashes) > 50:
            print(f"  ... and {len(crashes) - 50} more")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
