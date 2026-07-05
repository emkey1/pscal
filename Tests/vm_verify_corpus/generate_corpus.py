#!/usr/bin/env python3
"""Generates the Phase 1e verifier's malformed-.bc test corpus
(Docs/pscal_vm2_plan.md §5.5). Compiles small fixtures with the real Pascal
compiler (and, for the embedded-closure case, exsh) to get well-formed PSB3
chunks, then splices in specific corruptions (truncation, bad jump targets,
bad constant indices, a stack-underflow instruction sequence, an
unknown-region runtime-backstop underflow, a control-flow join-point
depth-check bypass, a malformed embedded shell-closure chunk, and a
self-attested trusted-skip-verify flag) so run_corpus_tests.py can assert
every one fails cleanly (no crash, nonzero exit) through pscalvm.

Usage: python3 generate_corpus.py [--pascal-bin PATH] [--exsh-bin PATH] [--out DIR]
"""

import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psb3  # noqa: E402
import psb3_value  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Opcode values from components/pscal-core/src/compiler/opcodes.def -- keep
# in sync if that file's ordinals ever change (they are pinned by
# _Static_asserts there, so this is low-risk).
OP_CONSTANT = 0x01
OP_CONSTANT16 = 0x02
OP_CONST_FALSE = 0x06
OP_ADD = 0x08
OP_JUMP_IF_FALSE = 0x1C
OP_JUMP = 0x1D
OP_CALL_HOST = 0x53
OP_HALT = 0x59
HOST_FN_QUIT_REQUESTED = 0  # first entry of HostFunctionID (vm/vm.h); always
                            # registered by vmInit regardless of frontend, and
                            # its C implementation touches no VM stack slots
                            # (see vmHostQuitRequested), so its net runtime
                            # stack effect is exactly "push one result" --
                            # deterministic and frontend-independent.

HELLO_SRC = """program Hello;
var i: integer;
function Square(x: integer): integer;
begin
  Square := x * x;
end;
begin
  for i := 1 to 5 do
    if i > 0 then
      writeln('square(', i, ')=', Square(i));
end.
"""

TRIVIAL_SRC = """program Trivial;
begin
  writeln('hi');
end.
"""

# A shell function is compiled to a ShellCompiledFunction (a TYPE_POINTER
# constant, kind==1) embedded in the *enclosing* chunk's constant pool -- see
# core/compiled_function.h and cache.c's writePointerValue/readPointerValue.
# This fixture just needs one function definition so its cached .bc has an
# embedded closure to corrupt (finding 3: the nested chunk bypassed
# verification entirely before the fix).
SHELL_CLOSURE_SRC = """myfunc() {
  echo "hello from closure"
}
myfunc
"""


def compile_to_bc(pascal_bin, source, cache_root):
    src_dir = tempfile.mkdtemp()
    src_path = os.path.join(src_dir, "prog.pas")
    with open(src_path, "w") as f:
        f.write(source)
    env = dict(os.environ)
    env["HOME"] = cache_root
    subprocess.run([pascal_bin, src_path], cwd=src_dir, env=env,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    matches = glob.glob(os.path.join(cache_root, ".pscal", "bc_cache", "*.bc"))
    if not matches:
        raise RuntimeError(f"no .bc produced for fixture under {cache_root}")
    # Freshest entry is ours; cache_root is a scratch dir used once per fixture.
    matches.sort(key=os.path.getmtime)
    with open(matches[-1], "rb") as f:
        return f.read()


def compile_shell_to_bc(exsh_bin, source, cache_root):
    src_dir = tempfile.mkdtemp()
    src_path = os.path.join(src_dir, "prog.exsh")
    with open(src_path, "w") as f:
        f.write(source)
    env = dict(os.environ)
    env["HOME"] = cache_root
    subprocess.run([exsh_bin, src_path], cwd=src_dir, env=env,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    matches = glob.glob(os.path.join(cache_root, ".pscal", "bc_cache", "*.bc"))
    if not matches:
        raise RuntimeError(f"no .bc produced for shell fixture under {cache_root}")
    matches.sort(key=os.path.getmtime)
    with open(matches[-1], "rb") as f:
        return f.read()


def find_first_opcode(code, wanted):
    """Walks `code` using each opcode's *fixed* operand length (good enough
    for these two small fixtures, which contain none of the four
    variable-length opcodes) and returns the offset of the first occurrence
    of any opcode in `wanted`, plus the still-unpatched raw code bytes."""
    # Minimal fixed-length table for the opcodes these fixtures can contain.
    fixed_len = {
        0x00: 1, 0x01: 2, 0x02: 3, 0x03: 1, 0x04: 1, 0x05: 1, 0x06: 1, 0x07: 2,
        0x08: 1, 0x09: 1, 0x0A: 1, 0x0B: 1, 0x0C: 1, 0x0D: 1, 0x0E: 1,
        0x0F: 1, 0x10: 1, 0x11: 1, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 1, 0x16: 1,
        0x17: 1, 0x18: 1, 0x19: 1, 0x1A: 1, 0x1B: 1, 0x1C: 5, 0x1D: 5, 0x1E: 1,
        0x1F: 1, 0x22: 10, 0x23: 10, 0x24: 2, 0x25: 11, 0x26: 11, 0x27: 3,
        0x28: 10, 0x29: 10, 0x2A: 11, 0x2B: 11, 0x2C: 2, 0x2D: 2, 0x2E: 2,
        0x2F: 2, 0x31: 4, 0x32: 3, 0x33: 2, 0x35: 2, 0x36: 2, 0x37: 2, 0x38: 2,
        0x39: 2, 0x3A: 3, 0x3B: 2, 0x3C: 3, 0x3D: 2, 0x3E: 3, 0x3F: 2, 0x40: 5,
        0x41: 2, 0x42: 5, 0x43: 1, 0x44: 1, 0x45: 1, 0x46: 1, 0x47: 1, 0x48: 1,
        0x49: 1, 0x4A: 2, 0x4B: 3, 0x4C: 2, 0x4D: 3, 0x4E: 2, 0x4F: 3,
        0x50: 4, 0x51: 5, 0x52: 4, 0x53: 2, 0x54: 1, 0x55: 8, 0x56: 2, 0x57: 3,
        0x58: 2, 0x59: 1, 0x5A: 1, 0x5B: 3, 0x5C: 5, 0x5D: 1, 0x5E: 1, 0x5F: 1,
        0x60: 1, 0x61: 1, 0x62: 1, 0x63: 2,
    }
    pc = 0
    while pc < len(code):
        op = code[pc]
        if op in wanted:
            return pc
        pc += fixed_len.get(op, 1)
    return None


def code_section_payload(code_section_bytes):
    count, pos = psb3.decode_varint(code_section_bytes, 0)
    return code_section_bytes[pos:pos + count]


def rebuild_code_section(new_code):
    return psb3.encode_varint(len(new_code)) + new_code


def write_corpus(out_dir, name, data, expect_ok, note):
    path = os.path.join(out_dir, name)
    with open(path, "wb") as f:
        f.write(data)
    return {"file": name, "expect_ok": expect_ok, "note": note}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pascal-bin", default=os.path.join(REPO_ROOT, "build", "bin", "pascal"))
    ap.add_argument("--exsh-bin", default=os.path.join(REPO_ROOT, "build", "bin", "exsh"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "corpus"))
    args = ap.parse_args()

    if not os.path.exists(args.pascal_bin):
        print(f"pascal binary not found at {args.pascal_bin}", file=sys.stderr)
        return 1

    os.makedirs(args.out, exist_ok=True)
    for f in glob.glob(os.path.join(args.out, "*.bc")):
        os.remove(f)

    with tempfile.TemporaryDirectory() as cache_a, tempfile.TemporaryDirectory() as cache_b:
        hello_bytes = compile_to_bc(args.pascal_bin, HELLO_SRC, cache_a)
        trivial_bytes = compile_to_bc(args.pascal_bin, TRIVIAL_SRC, cache_b)

    manifest = []

    # --- Controls: unmodified golden files must still load and run cleanly. ---
    manifest.append(write_corpus(args.out, "golden_hello.bc", hello_bytes, True,
                                  "unmodified control: must load and run"))
    manifest.append(write_corpus(args.out, "golden_trivial.bc", trivial_bytes, True,
                                  "unmodified control: must load and run"))

    # --- Truncation at several byte offsets. The container 8-byte-aligns
    # section bodies, so the file's own last few bytes can be pure padding
    # past every section's declared [offset, offset+length) -- truncating
    # only that padding is harmless and must still load cleanly. Compute the
    # true end of real content so the "cuts into real data" cases actually do. ---
    hdr_size = struct.calcsize("<IHHII")
    _, _, _, _, section_count = struct.unpack_from("<IHHII", hello_bytes, 0)
    content_end = 0
    p = hdr_size
    for _ in range(section_count):
        _sid, off, length = struct.unpack_from("<III", hello_bytes, p)
        p += 12
        content_end = max(content_end, off + length)

    for label, cut, expect_ok in [
        ("empty", 0, False),
        ("header_only", 8, False),
        ("mid_directory", 20, False),
        ("half", len(hello_bytes) // 2, False),
        ("last_content_byte", content_end - 1, False),
        ("trailing_padding_only", content_end, True),
    ]:
        cut = max(0, min(cut, len(hello_bytes)))
        note = (f"truncated to {cut}/{len(hello_bytes)} bytes"
                if not expect_ok else
                f"truncated to {cut}/{len(hello_bytes)} bytes (removes only trailing alignment padding)")
        manifest.append(write_corpus(args.out, f"truncated_{label}.bc", hello_bytes[:cut], expect_ok, note))

    # --- Bad jump target: patch a JUMP/JUMP_IF_FALSE displacement to point
    # far past the end of the code section. ---
    pf = psb3.read_psb3(os.path.join(args.out, "golden_hello.bc"))
    code = bytearray(code_section_payload(pf.section(psb3.SEC_CODE)))
    jmp_pc = find_first_opcode(bytes(code), {OP_JUMP_IF_FALSE, OP_JUMP})
    if jmp_pc is not None:
        code[jmp_pc + 1:jmp_pc + 5] = (0x7FFFFFF0).to_bytes(4, "big")
        mutated = pf.with_section(psb3.SEC_CODE, rebuild_code_section(bytes(code)))
        manifest.append(write_corpus(args.out, "bad_jump_target.bc", mutated.to_bytes(), False,
                                      f"JUMP*/JUMP_IF_FALSE at code pc {jmp_pc} retargeted out of range"))
    else:
        print("warning: no JUMP/JUMP_IF_FALSE found in hello fixture; skipping bad_jump_target.bc",
              file=sys.stderr)

    # --- Bad constant index: patch a CONSTANT/CONSTANT16 operand out of range. ---
    code2 = bytearray(code_section_payload(pf.section(psb3.SEC_CODE)))
    const_pc = find_first_opcode(bytes(code2), {OP_CONSTANT, OP_CONSTANT16})
    if const_pc is not None:
        if code2[const_pc] == OP_CONSTANT:
            code2[const_pc + 1] = 0xFF
        else:
            code2[const_pc + 1:const_pc + 3] = (0xFFFF).to_bytes(2, "big")
        mutated = pf.with_section(psb3.SEC_CODE, rebuild_code_section(bytes(code2)))
        manifest.append(write_corpus(args.out, "bad_const_index.bc", mutated.to_bytes(), False,
                                      f"CONSTANT*/at code pc {const_pc} index set out of range"))
    else:
        print("warning: no CONSTANT/CONSTANT16 found in hello fixture; skipping bad_const_index.bc",
              file=sys.stderr)

    # --- Stack underflow: replace the trivial fixture's whole program body
    # with ADD;HALT against an empty stack (ADD pops 2, nothing was pushed).
    # LINE is replaced to match the new 2-byte code length; trivial.pas has
    # no user procedures so PROC/CONS/BMAP/TYPE need no changes. ---
    tpf = psb3.read_psb3(os.path.join(args.out, "golden_trivial.bc"))
    new_code = bytes([OP_ADD, OP_HALT])
    new_lines = psb3.encode_varint(1) + psb3.encode_varint(0) + psb3.encode_svarint(1)
    mutated = tpf.with_section(psb3.SEC_CODE, rebuild_code_section(new_code))
    mutated = mutated.with_section(psb3.SEC_LINE, new_lines)
    manifest.append(write_corpus(args.out, "stack_underflow.bc", mutated.to_bytes(), False,
                                  "main body replaced with ADD;HALT against an empty stack"))

    # --- Finding 1 regression: unknown-region runtime backstop.
    # CALL_HOST taints the verifier's abstract depth to "unknown" (its net
    # stack effect can't be resolved statically), so the ADD right after it
    # is never checked at verify time -- by design; the runtime's own
    # bounds-checked push()/pop() are supposed to be the backstop for that
    # region. HOST_FN_QUIT_REQUESTED pushes exactly one result and touches no
    # other stack slots, so the real depth after CALL_HOST is deterministic
    # (1), one short of ADD's requirement (2): FAST_POP/FAST_PUSH used to have
    # no bounds check at all here, so the second pop walked vm->stackTop
    # below vm->stack -- real memory corruption, not just a wrong answer.
    # Must now fail cleanly (at runtime, since the verifier itself still
    # accepts this by design) instead of corrupting memory. ---
    host_underflow_code = bytes([OP_CALL_HOST, HOST_FN_QUIT_REQUESTED, OP_ADD, OP_HALT])
    host_underflow_lines = psb3.encode_varint(1) + psb3.encode_varint(0) + psb3.encode_svarint(1)
    mutated = tpf.with_section(psb3.SEC_CODE, rebuild_code_section(host_underflow_code))
    mutated = mutated.with_section(psb3.SEC_LINE, host_underflow_lines)
    manifest.append(write_corpus(args.out, "unknown_region_fast_pop_underflow.bc", mutated.to_bytes(), False,
                                  "CALL_HOST (taints depth unknown) then ADD; real depth after "
                                  "CALL_HOST is 1, short of ADD's required 2 -- must fail cleanly "
                                  "at runtime via the FAST_POP/FAST_PUSH bounds check, not corrupt "
                                  "memory below vm->stack"))

    # --- Finding 2 regression: control-flow join-point dataflow bug.
    # CONST_FALSE; JUMP_IF_FALSE -> L; CALL_HOST (taints unknown); falls
    # through to L: ADD; HALT. JUMP_IF_FALSE's own jump-target successor (L,
    # known depth 0) and CALL_HOST's fallthrough successor (L, unknown depth)
    # are both pushed to the verifier's worklist; LIFO popping visits the
    # fallthrough/unknown edge into L *first*. The old join-point logic only
    # ran req/underflow checks on a pc's first worklist visit, so the later,
    # perfectly checkable known-depth arrival (0, insufficient for ADD's
    # required 2) was silently accepted once L had already been marked
    # "seen" by the unknown arrival -- a join-point bypass independent of any
    # single opcode's own taint. Must now be rejected at verify time. ---
    jif_disp = (8 - 6).to_bytes(4, "big")  # target L=pc8 from (pc1+len5)=pc6
    join_code = bytes([OP_CONST_FALSE]) + bytes([OP_JUMP_IF_FALSE]) + jif_disp + \
        bytes([OP_CALL_HOST, HOST_FN_QUIT_REQUESTED]) + bytes([OP_ADD, OP_HALT])
    assert len(join_code) == 10 and join_code[8] == OP_ADD  # keep pc arithmetic honest
    join_lines = psb3.encode_varint(1) + psb3.encode_varint(0) + psb3.encode_svarint(1)
    mutated = tpf.with_section(psb3.SEC_CODE, rebuild_code_section(join_code))
    mutated = mutated.with_section(psb3.SEC_LINE, join_lines)
    manifest.append(write_corpus(args.out, "join_point_known_after_unknown.bc", mutated.to_bytes(), False,
                                  "known-depth-0 arrival at a join point reached first via an "
                                  "unknown-tainted edge must still be checked against ADD's "
                                  "required depth of 2, not silently accepted"))

    # --- Finding 4 regression: the self-attested trusted-skip-verify header
    # flag must no longer bypass verification. Reuses the stack_underflow
    # mutation (ADD;HALT against an empty stack) but also sets flags bit 0
    # (the old PSB3_FLAG_TRUSTED_SKIP_VERIFY) -- pre-fix this bit, read
    # straight out of the file's own bytes with no provenance binding, made
    # psb3VerificationSkipped() skip the verifier entirely, so this exact
    # malformed chunk would load and run (into finding 1's territory). Now
    # the bit is ignored outright: the malformed chunk must still be
    # rejected at verify time, identically to plain stack_underflow.bc. ---
    trusted_skip_mutated = tpf.with_section(psb3.SEC_CODE, rebuild_code_section(new_code))
    trusted_skip_mutated = trusted_skip_mutated.with_section(psb3.SEC_LINE, new_lines)
    trusted_skip_mutated = psb3.Psb3File(trusted_skip_mutated.format_version,
                                          trusted_skip_mutated.vm_version,
                                          0x1,  # old PSB3_FLAG_TRUSTED_SKIP_VERIFY bit
                                          trusted_skip_mutated.sections)
    manifest.append(write_corpus(args.out, "trusted_skip_flag_bypass.bc", trusted_skip_mutated.to_bytes(), False,
                                  "same corruption as stack_underflow.bc, but with the file's own "
                                  "header flags bit 0 set (the removed self-attested "
                                  "PSB3_FLAG_TRUSTED_SKIP_VERIFY) -- must still be rejected, proving "
                                  "the bit is no longer honored"))

    # --- Finding 3 regression: an embedded shell-closure's nested chunk must
    # be verified too, not just the top-level chunk. Compiles a tiny exsh
    # function definition to get a real cached .bc whose constant pool
    # embeds a ShellCompiledFunction (TYPE_POINTER, kind==1) closure, then
    # patches the *nested* chunk's first CONSTANT operand out of range
    # in-place (same corruption class as bad_const_index.bc, just inside the
    # embedded chunk instead of the top-level one). Before the fix, nothing
    # ever called pscalVerifyBytecodeChunk() on this nested chunk, so it
    # loaded successfully and only failed (via CONSTANT's completely
    # unchecked `vm->chunk->constants[idx]` read -- no bounds check at all)
    # when the closure was actually invoked. ---
    if not os.path.exists(args.exsh_bin):
        print(f"warning: exsh binary not found at {args.exsh_bin}; skipping "
              "nested_closure_bad_const.bc", file=sys.stderr)
    else:
        with tempfile.TemporaryDirectory() as cache_c:
            closure_bytes = compile_shell_to_bc(args.exsh_bin, SHELL_CLOSURE_SRC, cache_c)
        # psb3.read_psb3() only takes a path; round-trip through a scratch file.
        closure_path = os.path.join(args.out, "_closure_fixture.bc")
        with open(closure_path, "wb") as f:
            f.write(closure_bytes)
        cpf = psb3.read_psb3(closure_path)
        os.remove(closure_path)
        cons_bytes = bytearray(cpf.section(psb3.SEC_CONS))
        nested_code_start, nested_code_len = psb3_value.find_first_embedded_closure_code(bytes(cons_bytes))
        nested_code = bytes(cons_bytes[nested_code_start:nested_code_start + nested_code_len])
        bad_pc = find_first_opcode(nested_code, {OP_CONSTANT, OP_CONSTANT16})
        if bad_pc is None:
            print("warning: no CONSTANT/CONSTANT16 found in embedded closure body; skipping "
                  "nested_closure_bad_const.bc", file=sys.stderr)
        else:
            if nested_code[bad_pc] == OP_CONSTANT:
                cons_bytes[nested_code_start + bad_pc + 1] = 0xFF
            else:
                cons_bytes[nested_code_start + bad_pc + 1:nested_code_start + bad_pc + 3] = \
                    (0xFFFF).to_bytes(2, "big")
            mutated = cpf.with_section(psb3.SEC_CONS, bytes(cons_bytes))
            manifest.append(write_corpus(args.out, "nested_closure_bad_const.bc", mutated.to_bytes(), False,
                                          f"embedded shell-closure's nested CONSTANT* at inner code pc "
                                          f"{bad_pc} index set out of range; the enclosing chunk is "
                                          "well-formed, only the nested chunk is corrupt"))

    manifest_path = os.path.join(args.out, "manifest.json")
    import json
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"wrote {len(manifest)} corpus files to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
