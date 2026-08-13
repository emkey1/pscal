#!/usr/bin/env python3
"""VM 2.0 Phase 3 (plan Docs/pscal_vm2_plan.md §5.9): the operand stack's
VM_STACK_MAX ceiling is no longer a fixed array bound -- it is a
runtime-configurable value (PSCAL_VM_MAX_STACK_VALUES), read by both the
runtime's push()/vmFastPushUnchecked() growth path *and* the Phase 1e
verifier's per-procedure abstract-stack-depth check (bytecode_verify.c,
via the shared pscalVmStackCeilingValues() accessor in vm.h/vm.c) so the
two never disagree. This is a standalone script rather than a
generate_corpus.py manifest entry because exercising it needs a per-case
environment override, which the manifest/run_corpus_tests.py pipeline
does not model -- a 1M+-entry declared-depth fixture large enough to
exceed the *default* ceiling would be an impractically large corpus file
to commit, whereas a tiny fixture plus a lowered PSCAL_VM_MAX_STACK_VALUES
exercises the identical code path.

Builds a chunk whose single procedure pushes N constants with no pops
(declared abstract depth = N), then runs pscalvm against it twice:
  1. PSCAL_VM_MAX_STACK_VALUES set below N -> must be rejected cleanly at
     *load/verify* time (nonzero exit, no crash), proving the verifier
     honors the override, not a hardcoded compile-time constant.
  2. PSCAL_VM_MAX_STACK_VALUES left at its generous default -> must load
     and run to completion (the chunk is otherwise well-formed).

Usage: python3 test_stack_ceiling.py [--pascal-bin PATH] [--pscalvm-bin PATH]
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psb3  # noqa: E402
from generate_corpus import (  # noqa: E402
    TRIVIAL_SRC, compile_to_bc, code_section_payload, rebuild_code_section,
    OP_CONSTANT, OP_HALT,
)

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEPTH = 32  # small, but pushes+HALT with no pops -> declared depth == DEPTH


def build_fixture(pascal_bin):
    cache_root = tempfile.mkdtemp()
    trivial_bytes = compile_to_bc(pascal_bin, TRIVIAL_SRC, cache_root)
    trivial_path = os.path.join(cache_root, "_trivial.bc")
    with open(trivial_path, "wb") as f:
        f.write(trivial_bytes)
    pf = psb3.read_psb3(trivial_path)
    os.remove(trivial_path)

    # Constant index 0 always exists in a compiled chunk (at minimum the
    # program's own literals); its *value* is irrelevant here, only that
    # CONSTANT is a valid, verifier-recognized "push one" opcode.
    new_code = bytes([OP_CONSTANT, 0x00] * DEPTH) + bytes([OP_HALT])
    new_lines = psb3.encode_varint(1) + psb3.encode_varint(0) + psb3.encode_svarint(1)
    mutated = pf.with_section(psb3.SEC_CODE, rebuild_code_section(new_code))
    mutated = mutated.with_section(psb3.SEC_LINE, new_lines)
    return mutated.to_bytes()


def run_pscalvm(pscalvm_bin, bc_path, max_stack_values):
    env = dict(os.environ)
    if max_stack_values is not None:
        env["PSCAL_VM_MAX_STACK_VALUES"] = str(max_stack_values)
    else:
        env.pop("PSCAL_VM_MAX_STACK_VALUES", None)
    proc = subprocess.run([pscalvm_bin, bc_path], env=env,
                           capture_output=True, timeout=30)
    return proc.returncode, proc.stdout, proc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pascal-bin", default=os.path.join(REPO_ROOT, "build", "bin", "pascal"))
    ap.add_argument("--pscalvm-bin", default=os.path.join(REPO_ROOT, "build", "bin", "pscalvm"))
    args = ap.parse_args()

    fixture_bytes = build_fixture(args.pascal_bin)
    tmp_dir = tempfile.mkdtemp()
    bc_path = os.path.join(tmp_dir, "stack_ceiling_probe.bc")
    with open(bc_path, "wb") as f:
        f.write(fixture_bytes)

    failures = []

    # Case 1: ceiling below the declared depth -> clean rejection.
    rc, out, err = run_pscalvm(args.pscalvm_bin, bc_path, DEPTH - 1)
    if rc == 0:
        failures.append(f"lowered ceiling ({DEPTH - 1} < declared depth {DEPTH}): "
                         f"expected nonzero exit, got 0 (stdout={out!r} stderr={err!r})")
    elif rc < 0:
        failures.append(f"lowered ceiling: process died from signal {-rc} "
                         f"(crash, not a clean rejection); stderr={err!r}")
    else:
        print(f"[PASS] lowered ceiling ({DEPTH - 1}) rejected cleanly, exit={rc}")

    # Case 2: default (generous) ceiling -> loads and runs fine.
    rc, out, err = run_pscalvm(args.pscalvm_bin, bc_path, None)
    if rc != 0:
        failures.append(f"default ceiling: expected exit 0, got {rc} "
                         f"(stdout={out!r} stderr={err!r})")
    else:
        print("[PASS] default ceiling: loads and runs to completion")

    if failures:
        for f in failures:
            print(f"[FAIL] {f}", file=sys.stderr)
        return 1
    print("Ran 2 stack-ceiling check(s); 0 failure(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
