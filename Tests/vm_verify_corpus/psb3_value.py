"""Minimal parser for the CONS section's Value-record encoding (VM 2.0 plan,
Docs/pscal_vm2_plan.md §5.2; format documented in
Docs/pscal_vm_manual/pscal_vm_manual_ch2.md §2.2). Mirrors
components/pscal-core/src/core/cache.c's writeValue()/readValue() /
writePointerValue()/readPointerValue() byte-for-byte, but only far enough to
*locate* an embedded shell-closure's nested CODE bytes (kind==1 of
TYPE_POINTER) for corpus generation -- it does not attempt to fully parse a
nested chunk's LINE/PROC/TYPE sections, since callers only need the CODE
sub-blob's bounds within the enclosing CONS buffer.

Keep in sync with cache.c if the Value wire format ever changes.
"""

import struct

import psb3

# VarType ordinals from components/pscal-core/src/core/types.h (0-indexed enum).
(T_UNKNOWN, T_VOID, T_INT32, T_DOUBLE, T_STRING, T_CHAR, T_RECORD, T_FILE, T_BYTE,
 T_WORD, T_ENUM, T_ARRAY, T_BOOLEAN, T_MEMORYSTREAM, T_SET, T_POINTER, T_INTERFACE,
 T_CLOSURE, T_INT8, T_UINT8, T_INT16, T_UINT16, T_UINT32, T_INT64, T_UINT64, T_FLOAT,
 T_LONG_DOUBLE, T_NIL, T_THREAD, T_WIDECHAR, T_UNICODE_STRING) = range(31)

_INT_LIKE_U64 = {T_INT32, T_WORD, T_BYTE, T_BOOLEAN, T_INT8, T_INT16, T_INT64}
_UINT_LIKE_U64 = {T_UINT8, T_UINT16, T_UINT32, T_UINT64}


class EmbeddedClosure(Exception):
    """Raised (as a control-flow signal) with the nested CODE bounds of the
    first kind==1 (embedded ShellCompiledFunction) TYPE_POINTER constant
    found while walking a CONS section."""

    def __init__(self, code_start, code_len):
        super().__init__()
        self.code_start = code_start
        self.code_len = code_len


def _skip_value(data, pos):
    """Advances past one Value record starting at `pos`. Raises
    EmbeddedClosure the moment it sees a kind==1 pointer (callers that want
    to locate one should call find_first_embedded_closure_code() instead of
    using this directly)."""
    vtype = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    if vtype in _INT_LIKE_U64 or vtype in _UINT_LIKE_U64:
        pos += 8
    elif vtype == T_FLOAT:
        pos += 4
    elif vtype == T_DOUBLE:
        pos += 8
    elif vtype == T_LONG_DOUBLE:
        pos += 8
    elif vtype == T_CHAR:
        pos += 4
    elif vtype == T_STRING:
        present = data[pos]
        pos += 1
        if present:
            ln, pos = psb3.decode_varint(data, pos)
            pos += ln
    elif vtype == T_NIL:
        pass
    elif vtype == T_ENUM:
        ln, pos = psb3.decode_varint(data, pos)
        pos += ln
        pos += 4  # ordinal i32
    elif vtype == T_SET:
        sz = struct.unpack_from("<i", data, pos)[0]
        pos += 4
        pos += 8 * sz
    elif vtype == T_POINTER:
        kind = data[pos]
        pos += 1
        if kind == 0:
            pass
        elif kind == 1:
            pos += 4  # chunk->version, u32le
            code_len, code_start = psb3.decode_varint(data, pos)
            raise EmbeddedClosure(code_start, code_len)
        elif kind == 2:
            ln, pos = psb3.decode_varint(data, pos)
            pos += ln
        elif kind == 3:
            pos += 8
        else:
            raise ValueError(f"unknown pointer kind {kind} at pos {pos}")
    else:
        raise ValueError(f"unhandled/unexpected constant-pool VarType {vtype} at pos {pos} "
                          "(only simple top-level constant shapes are supported by this probe)")
    return pos


def find_first_embedded_closure_code(cons_bytes):
    """Walks a top-level CONS section body and returns (code_start, code_len)
    -- byte offsets *within cons_bytes* -- for the first embedded
    ShellCompiledFunction's nested CODE payload. Raises RuntimeError if none
    is found."""
    count, pos = psb3.decode_varint(cons_bytes, 0)
    for _ in range(count):
        try:
            pos = _skip_value(cons_bytes, pos)
        except EmbeddedClosure as found:
            return found.code_start, found.code_len
    raise RuntimeError("no embedded shell-closure (TYPE_POINTER kind==1) constant found")
