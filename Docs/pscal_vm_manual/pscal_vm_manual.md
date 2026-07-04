# PSCAL Virtual Machine Technical Manual

A production-grade reference for the PSCAL bytecode engine: the 64-bit
stack-based virtual machine (`components/pscal-core/src/vm/`) shared by the
Pascal, Rea, CLike, Aether, and exsh frontends. Every struct layout, opcode
encoding, constant, and worked example in this manual was taken from — and
where executable, verified against — the current source tree and `build/bin`
binaries, not reconstructed from generic VM lore.

## How the pieces fit

```mermaid
graph LR
    FE["Frontends\npascal · rea · clike · aether · exsh"]
    AST["AST\n(--dump-ast-json ⇄ pscaljson2bc)"]
    BC["PSB2 bytecode\n(.bc / cache)"]
    VM["interpretBytecode()\n100-opcode stack VM"]
    NB["Builtin registry\nHTTP/TLS · sockets · SQLite · JSON · AI · custom"]

    FE --> AST --> BC --> VM --> NB
```

## Chapters

### [Chapter 1 — System Architecture & Runtime State](pscal_vm_manual_ch1.md)
The execution loop (`interpretBytecode()`, computed-goto dispatch, the
polymorphic `BINARY_OP` fetch-decode-execute path); the memory model
(fixed 8192-`Value` operand stack, 4096-deep `CallFrame` windows, dual
global tables); builtins as the side-effect gateway **and the VM's extension
seam** (§1.3); real pthreads multithreading — per-thread VM instances, the
16-slot worker pool, mutex opcodes, cooperative pause/cancel/kill (§1.4).

### [Chapter 2 — The Bytecode & Binary File Specification](pscal_vm_manual_ch2.md)
The PSB2 container: magic `0x50534232`, format version 9, FNV-1a integrity
hashes; byte-exact layout of the code segment, constant pool
(`writeValue()` encodings for every `VarType`), builtin lowercase map,
procedure metadata, and type table; the `pscaljson2bc` AST-JSON pipeline;
a worked example run end-to-end (Pascal source → JSON → `.bc` hexdump →
disassembly → execution).

### [Chapter 3 — The Instruction Set Architecture Reference](pscal_vm_manual_ch3.md)
All 100 opcodes (`0x00`–`0x63`) in category tables: hex, mnemonic, exact
operand encoding, Forth-style stack effect, and mechanics. Covers the
8-byte self-patching inline caches in the code stream, big-endian operand
convention, the seven call forms, JSON handle semantics (builtins, not
opcodes), threading/mutex opcodes, and a byte-for-byte validation against
the Chapter 2 disassembly.

### [Chapter 4 — Built-in Subsystems & Native Bindings](pscal_vm_manual_ch4.md)
The extensibility model: `registerVmBuiltin()` and why one C registration
is inherited by all five frontend languages; the HTTP/TLS engine
(32-session pool, secure-by-default TLS, the mirror-copy async job layer
with cancel/progress, sequence and state diagrams); sockets/DNS; the
SQLite and yyjson handle runtimes; the OpenAI chat builtin as a
composition case study; the four uniform rules native subsystems follow.

## Errata guarded against

Facts in this manual that commonly circulate incorrectly, pinned here from
source: the ISA has **100** opcodes (not 141); instruction-stream operands
are **big-endian** while the PSB2 container header is host-little-endian;
there is **no** `fx`/effect-boundary construct, no `@pre`/`@post` opcode,
and no "TOON" handle system (the structured-data layer is yyjson-backed
JSON handles); SQLite bindings **do** exist (`ENABLE_EXT_BUILTIN_SQLITE`);
and `pscaljson2bc` **does** exist — its sources live in the umbrella repo
(`src/tools/`), not in pscal-core.
