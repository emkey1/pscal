#!/usr/bin/env python3
"""Compare two PSB3 bytecode files section by section.

Usage: psb3_compare.py [--ignore FOURCC]... [--types-as-loaded] A.bc B.bc

Exits 0 when the headers agree and every section not ignored is byte-identical
in both files (a section missing from one counts as a difference). Otherwise
prints one line per difference and exits 1. The container format is read with
Tests/vm_verify_corpus/psb3.py, which mirrors core/cache.c.

--types-as-loaded compares the TYPE section the way the loader sees it:
readTypesSection() hands each entry to insertType(), which keeps one entry per
name (compared without case, as findTypeEntry() does) and lets the last
definition win. A frontend can register a name twice,
and a rebuilt chunk lists it once, so the bytes differ while the loaded type
tables are the same.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "vm_verify_corpus"))
import psb3  # noqa: E402


def fourcc(section_id):
    return struct.pack("<I", section_id).decode("ascii", "replace")


def skip_ast(data, pos):
    """Return the offset just past one AST as core/cache.c's writeAst() lays it out."""
    present = data[pos]
    pos += 1
    if not present:
        return pos
    pos += 4 + 4 + 1  # node type, var type, flags
    has_token = data[pos]
    pos += 1
    if has_token:
        pos += 4  # token type
        length, pos = psb3.decode_varint(data, pos)
        pos += length
    pos += 4  # i_val
    for _ in range(3):  # left, right, extra
        pos = skip_ast(data, pos)
    (child_count,) = struct.unpack_from("<i", data, pos)
    pos += 4
    for _ in range(child_count):
        pos = skip_ast(data, pos)
    return pos


def types_as_loaded(data):
    """TYPE section bytes -> {lowercased name: AST bytes}, last definition winning."""
    count, pos = psb3.decode_varint(data, 0)
    table = {}
    for _ in range(count):
        length, pos = psb3.decode_varint(data, pos)
        name = data[pos:pos + length].lower()
        pos += length
        end = skip_ast(data, pos)
        table[name] = data[pos:end]
        pos = end
    return table


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ignore", action="append", default=[],
                        metavar="FOURCC", help="section to leave out, e.g. META")
    parser.add_argument("--types-as-loaded", action="store_true",
                        help="compare TYPE by name, last definition winning")
    parser.add_argument("a")
    parser.add_argument("b")
    args = parser.parse_args()

    ignored = {struct.unpack("<I", name.encode("ascii"))[0] for name in args.ignore}
    a, b = psb3.read_psb3(args.a), psb3.read_psb3(args.b)

    problems = []
    for field in ("format_version", "vm_version", "flags"):
        va, vb = getattr(a, field), getattr(b, field)
        if va != vb:
            problems.append(f"header {field}: {va} vs {vb}")

    sa = {sid: data for sid, data in a.sections if sid not in ignored}
    sb = {sid: data for sid, data in b.sections if sid not in ignored}
    for sid in sorted(set(sa) | set(sb)):
        da, db = sa.get(sid), sb.get(sid)
        if da is None or db is None:
            problems.append(f"section {fourcc(sid)} present only in "
                            f"{args.a if db is None else args.b}")
        elif sid == psb3.SEC_TYPE and args.types_as_loaded:
            ta, tb = types_as_loaded(da), types_as_loaded(db)
            for name in sorted(set(ta) | set(tb)):
                label = name.decode("utf-8", "replace")
                if name not in ta or name not in tb:
                    problems.append(f"type {label} present only in "
                                    f"{args.a if name in ta else args.b}")
                elif ta[name] != tb[name]:
                    problems.append(f"type {label} differs once loaded")
        elif da != db:
            first = next((i for i, (x, y) in enumerate(zip(da, db)) if x != y),
                         min(len(da), len(db)))
            problems.append(f"section {fourcc(sid)} differs: {len(da)} vs {len(db)} "
                            f"bytes, first difference at byte {first}")

    for line in problems:
        print(line)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
