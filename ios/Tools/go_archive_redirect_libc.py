#!/usr/bin/env python3
"""Point a Go c-archive's own calls to libc's open, openat, mkdir and mkdirat at PSCAL's shims.

The Go runtime reaches libc directly, on threads no task runs on, so nothing of the app's file
mask applies to what a Go program makes: the host's mask does. In the archive's go.o each of
these is one undefined symbol that every call relocates against. This renames those symbols
(_open to _pscal_go_open, and so on), so the final link binds them to smallclue's micro_app.c,
which applies the mask of the task that started the program. Nothing else in the app changes.

Usage: go_archive_redirect_libc.py ARCHIVE
Exits 1 if go.o is missing, is not a 64-bit Mach-O object, or still names a symbol afterwards.
"""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

RENAMES = {
    b"_open": b"_pscal_go_open",
    b"_openat": b"_pscal_go_openat",
    b"_mkdir": b"_pscal_go_mkdir",
    b"_mkdirat": b"_pscal_go_mkdirat",
}
MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
N_TYPE = 0x0E
N_UNDF = 0x0
N_EXT = 0x01
NLIST_64 = struct.Struct("<IBBHQ")


def fail(message: str) -> None:
    print(f"[go-redirect] error: {message}", file=sys.stderr)
    sys.exit(1)


def undefined_names(data: bytes) -> set:
    return {name for name, _ in symbols(data)}


def symbols(data: bytes):
    """Yield (name, index of its nlist) for each undefined external symbol."""
    symoff, nsyms, stroff, _ = symtab(data)
    for i in range(nsyms):
        strx, ntype, _, _, _ = NLIST_64.unpack_from(data, symoff + i * NLIST_64.size)
        if (ntype & N_TYPE) == N_UNDF and (ntype & N_EXT) and strx:
            end = data.index(b"\0", stroff + strx)
            yield data[stroff + strx:end], i


def symtab(data: bytes):
    magic, _, _, filetype, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != MH_MAGIC_64 or filetype != 1:
        fail("go.o is not a 64-bit Mach-O object")
    offset = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, offset)
        if cmd == LC_SYMTAB:
            return struct.unpack_from("<IIII", data, offset + 8)
        offset += size
    fail("go.o has no symbol table")


def symtab_command_offset(data: bytes) -> int:
    ncmds = struct.unpack_from("<I", data, 16)[0]
    offset = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, offset)
        if cmd == LC_SYMTAB:
            return offset
        offset += size
    fail("go.o has no symbol table")
    return -1


def redirect(data: bytes) -> bytes:
    symoff, nsyms, stroff, strsize = symtab(data)
    targets = [(name, index) for name, index in symbols(data) if name in RENAMES]
    if not targets:
        return data
    out = bytearray(data)
    # The new names go after the old table, moved to the end of the file if it is not there.
    table = bytearray(data[stroff:stroff + strsize])
    new_offsets = {}
    for name, _ in targets:
        if name not in new_offsets:
            new_offsets[name] = len(table)
            table += RENAMES[name] + b"\0"
    while len(table) % 8:
        table += b"\0"
    if stroff + strsize == len(data):
        new_stroff = stroff
        del out[stroff:]
    else:
        new_stroff = (len(out) + 7) & ~7
        out += b"\0" * (new_stroff - len(out))
    out += table
    cmd = symtab_command_offset(data)
    struct.pack_into("<IIII", out, cmd + 8, symoff, nsyms, new_stroff, len(table))
    for name, index in targets:
        struct.pack_into("<I", out, symoff + index * NLIST_64.size, new_offsets[name])
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().split("\n\n")[2], file=sys.stderr)
        return 2
    archive = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as work:
        subprocess.run(["ar", "x", str(archive), "go.o"], cwd=work, check=True)
        member = Path(work) / "go.o"
        if not member.exists():
            fail(f"no go.o in {archive}")
        data = member.read_bytes()
        patched = redirect(data)
        left = undefined_names(patched) & set(RENAMES)
        if left:
            fail("go.o still names " + ", ".join(sorted(n.decode() for n in left)))
        if b"_pscal_go_open" not in undefined_names(patched):
            fail("go.o never called open: is this a Go archive?")
        if patched == data:
            print(f"[go-redirect] {archive.name}: already redirected")
            return 0
        member.write_bytes(patched)
        subprocess.run(["ar", "r", str(archive), "go.o"], cwd=work, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["ranlib", str(archive)], check=True, stderr=subprocess.DEVNULL)
    names = sorted(n.decode() for n in undefined_names(patched) if n.startswith(b"_pscal_go_"))
    print(f"[go-redirect] {archive.name}: go.o now calls {', '.join(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
