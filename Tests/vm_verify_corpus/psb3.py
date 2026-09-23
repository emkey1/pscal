"""Minimal PSB3 container reader/writer (VM 2.0 plan, Docs/pscal_vm2_plan.md
§5.2/§5.5), just enough to splice malformed sections into a real chunk for
Phase 1e verifier corpus generation. Mirrors components/pscal-core/src/core/cache.c
byte-for-byte; keep in sync if that format ever changes.
"""

import struct

MAGIC = 0x50534233  # 'PSB3'
SECTION_ALIGN = 8

# The container-format epoch cache.c's PSB3_FORMAT_VERSION must agree with.
# read_psb3() carries whatever a real file says, so this is only needed when
# synthesising a header from nothing; it is asserted against a real file by
# check_format_version() so a bump on the C side can't go unnoticed here.
FORMAT_VERSION = 5

# Header `flags` bits 0..7 carry the FrontendKind the chunk was compiled
# under (cache.c's PSB3_FLAG_FRONTEND_MASK, common/frontend_kind.h). This is
# what lets `pscalvm prog.bc` apply the right string-index base and
# diagnostics instead of assuming Pascal. Bits 8..31 are reserved and 0.
FLAG_FRONTEND_MASK = 0x000000FF

FRONTEND_UNKNOWN = 0
FRONTEND_PASCAL = 1
FRONTEND_REA = 2
FRONTEND_AETHER = 3
FRONTEND_CLIKE = 4
FRONTEND_SHELL = 5
FRONTEND_LAST = FRONTEND_SHELL

SEC_CODE = struct.unpack("<I", b"CODE")[0]
SEC_LINE = struct.unpack("<I", b"LINE")[0]
SEC_CONS = struct.unpack("<I", b"CONS")[0]
SEC_PROC = struct.unpack("<I", b"PROC")[0]
SEC_TYPE = struct.unpack("<I", b"TYPE")[0]
SEC_BMAP = struct.unpack("<I", b"BMAP")[0]
SEC_META = struct.unpack("<I", b"META")[0]


def encode_varint(value):
    out = bytearray()
    v = value
    while True:
        byte = v & 0x7F
        v >>= 7
        if v:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            break
    return bytes(out)


def encode_svarint(value):
    zigzag = (value << 1) ^ (value >> 63) if value < 0 else (value << 1)
    return encode_varint(zigzag & 0xFFFFFFFFFFFFFFFF)


def decode_varint(data, pos):
    result = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            break
        shift += 7
    return result, pos


class Psb3File:
    def __init__(self, format_version, vm_version, flags, sections):
        self.format_version = format_version
        self.vm_version = vm_version
        self.flags = flags
        # sections: ordered dict-like list of (id:int, data:bytes)
        self.sections = sections

    def section(self, sec_id):
        for sid, data in self.sections:
            if sid == sec_id:
                return data
        raise KeyError(sec_id)

    @property
    def frontend(self):
        """The FrontendKind code in the header's flags word."""
        return self.flags & FLAG_FRONTEND_MASK

    def with_flags(self, new_flags):
        return Psb3File(self.format_version, self.vm_version, new_flags, self.sections)

    def with_frontend(self, kind):
        """A copy whose header names `kind`, leaving the reserved bits alone."""
        return self.with_flags((self.flags & ~FLAG_FRONTEND_MASK) | (kind & FLAG_FRONTEND_MASK))

    def with_section(self, sec_id, new_data):
        new_sections = [
            (sid, new_data if sid == sec_id else data) for sid, data in self.sections
        ]
        return Psb3File(self.format_version, self.vm_version, self.flags, new_sections)

    def to_bytes(self):
        n = len(self.sections)
        header = struct.pack("<IHHII", MAGIC, self.format_version, self.vm_version, self.flags, n)
        dir_entry_size = 12
        running = len(header) + n * dir_entry_size
        running = (running + SECTION_ALIGN - 1) & ~(SECTION_ALIGN - 1)
        offsets = []
        for _sid, data in self.sections:
            offsets.append(running)
            running += len(data)
            running = (running + SECTION_ALIGN - 1) & ~(SECTION_ALIGN - 1)
        directory = bytearray()
        for (sid, data), off in zip(self.sections, offsets):
            directory += struct.pack("<III", sid, off, len(data))
        out = bytearray(header)
        out += directory
        for (sid, data), off in zip(self.sections, offsets):
            while len(out) < off:
                out.append(0)
            out += data
            pad = (-(len(out)) % SECTION_ALIGN)
            out += b"\x00" * pad
        return bytes(out)


def read_psb3(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, format_ver, vm_ver, flags, section_count = struct.unpack_from("<IHHII", data, 0)
    if magic != MAGIC:
        raise ValueError(f"not a PSB3 file: {path}")
    pos = struct.calcsize("<IHHII")
    entries = []
    for _ in range(section_count):
        sid, off, length = struct.unpack_from("<III", data, pos)
        pos += 12
        entries.append((sid, off, length))
    sections = [(sid, data[off:off + length]) for sid, off, length in entries]
    return Psb3File(format_ver, vm_ver, flags, sections)


def check_format_version(path):
    """Raise if `path`'s container epoch is not the one this module mirrors.

    This module reimplements cache.c's container byte for byte, so a
    PSB3_FORMAT_VERSION bump on the C side that isn't reflected here would
    silently start producing files the real reader rejects -- and the corpus
    would read that as "the verifier caught it", passing for the wrong reason.
    """
    actual = read_psb3(path).format_version
    if actual != FORMAT_VERSION:
        raise ValueError(
            f"{path} is container format {actual}, but psb3.py mirrors format "
            f"{FORMAT_VERSION}; update this module to match cache.c"
        )
