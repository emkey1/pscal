# PSCAL Virtual Machine Technical Manual

## Chapter 2: The Bytecode & Binary File Specification

> Source of truth for this chapter: `components/pscal-core/src/core/cache.c`
> (serialization/deserialization), `components/pscal-core/src/compiler/bytecode.h`
> (`BytecodeChunk`), `components/pscal-core/src/compiler/opcodes.def` (opcode
> page and operand-width specs), `components/pscal-core/src/core/version.h`
> (VM version), and the umbrella repo's `src/tools/json2bc.c` +
> `src/tools/ast_json_loader.c` (the `pscaljson2bc` tool). The worked example
> at the end of this chapter was generated and executed against the current
> `build/bin` binaries, not mocked.
>
> This chapter documents **PSB3**, the container format introduced in VM 2.0
> Phase 1b (`Docs/pscal_vm2_plan.md` §5.2). It is a hard cutover: the PSB2
> reader/writer this chapter used to describe are gone from the source tree,
> not kept as a fallback. A PSB2 cache entry left over from before the
> upgrade simply misses the magic check below and recompiles — the cache's
> normal cold path, not an error.

### 2.0 One Format, Two Consumers

PSCAL has a single on-disk bytecode container format with two consumers:

1. **The bytecode cache** (`saveBytecodeToCache`) — every frontend
   transparently caches compiled chunks under a per-compiler cache directory,
   keyed by `<compiler_id>-<basename>-<path_hash>-<source_hash>-<combined_hash>.bc`.
   Cache freshness requires the cache file's mtime to be *strictly newer*
   than the source (whole-second resolution, `isCacheFresh()`), plus hash
   verification on load via the `META` section (§2.2).
2. **Explicit `.bc` files** (`saveBytecodeToFile` / `pscalvm out.bc`) — the
   same container, minus the `META` section, written to a user-chosen path,
   executable directly by `pscalvm` and disassemblable by `pscald`. Omitting
   `META` means a distributable `.bc` no longer embeds the absolute local
   source path it was compiled from.

Both paths share one writer (`psb3Write()`) and one section-body-reader set
(`psb3ReadChunk()`/`readCodeSection()`/etc.), so there is exactly one format
to document.

### 2.1 Container Header and Section Directory

The header is written by `psb3Write()` (`cache.c`):

```
[magic   u32le]  0x50534233 ('PSB3'; little-endian bytes on disk read "3BSP")
[format_ver u16] container-format version (currently 3 as of VM 2.0 Phase 2b,
                 up from 1; independent of the VM semantic version below —
                 this axis can move without a VM version bump. The 1→2 bump
                 was CODE's cache_count field, §2.2 (the first section-shape
                 change since PSB3's introduction); 2→3 changed no section
                 shape at all -- it exists purely to invalidate stale
                 Phase-2a-vintage .bc entries whose CODE bytes use opcodes
                 (GET_GLOBAL/SET_GLOBAL/DEFINE_GLOBAL/...) that are
                 *structurally* still valid-looking under the new binary
                 (their opcodes.def entries are kept for legacy disassembly)
                 but are no longer executable -- see §2.2's CODE entry)
[vm_ver  u16]    chunk->version (PSCAL_VM_VERSION at compile time)
[flags   u32]    reserved, always 0 in this phase
[section_count u32]
section_count × { id:u32  offset:u32  length:u32 }   ; the section directory
<pad to 8-byte boundary>
section bodies, each starting on an 8-byte boundary
```

Every multi-byte integer in the container — the header, the directory, and
every section body — goes through explicit little-endian helpers
(`bufU16LE`/`bufU32LE`/`bufU64LE` on write, `curU16LE`/`curU32LE`/`curU64LE`
on read in `cache.c`), so the format is portable across host byte order and
host integer width. This replaces PSB2's raw `fwrite`/`fread` of
host-native ints, which was **not** portable (`components/pscal-core`'s
target fleet is little-endian everywhere PSCAL ships, so this was latent
until it wasn't — see `Docs/pscal_vm2_plan.md` §9 risk register).

Section ids are four ASCII bytes read in file order (a "FourCC"), matching
the current seven sections:

| id (on disk) | Constant | Contents |
|---|---|---|
| `CODE` | `PSB3_SEC_CODE` | Raw instruction bytes (§2.2) |
| `LINE` | `PSB3_SEC_LINE` | Varint line-run table (§2.2) |
| `CONS` | `PSB3_SEC_CONS` | Constant pool (§2.2) |
| `BMAP` | `PSB3_SEC_BMAP` | Builtin lowercase-name map + fingerprint (§2.2) |
| `PROC` | `PSB3_SEC_PROC` | Procedure metadata + const-global symbols (§2.2) |
| `TYPE` | `PSB3_SEC_TYPE` | Named type table (§2.2) |
| `META` | `PSB3_SEC_META` | Cache-only: source path + integrity hashes (§2.2); absent from explicit `.bc` files |

Unknown section ids are simply never looked up by the reader, so a future
phase can add an 8th section without another format break — a loader built
before that section existed just ignores it.

**Loader hardening (VM 2.0 Phase 1d).** Every read goes through a
bounds-checked `Cursor` (`cache.c`: `curU8`/`curU16LE`/`curVarint`/etc.) that
sticks a sticky `error` flag on the first out-of-bounds access instead of
reading past a buffer. Before any section body is parsed, `psb3ParseHeader()`
validates that every directory entry's `[offset, offset+length)` range is
fully inside the file and that no two sections overlap; a corrupt or
truncated `.bc` file fails the load cleanly (`INTERPRET_COMPILE_ERROR`-style
rejection, never undefined behavior). This has been fuzz-tested by flipping
every single byte and truncating at every 8-byte boundary of a real `.bc`
file against `pscalvm`: zero crashes, only clean errors or (when a flipped
byte happened not to matter) unchanged output.

### 2.2 Section Bodies

**CODE** (`writeCodeSection`/`readCodeSection`): `[varint byte_count]
[varint cache_count][byte_count raw bytes]`. The raw instruction stream —
opcode bytes and their operands, exactly as produced by the compiler (§2.3 is
unchanged: the compiler doesn't know or care about the container). This
section's own length prefix and the directory's recorded length are
redundant by construction (defense in depth): a reader can trust either, and
a mismatch between them is a corruption signal.

`cache_count` (VM 2.0 Phase 2a, `Docs/pscal_vm2_plan.md` §5.6) was the number
of `GET_GLOBAL`/`SET_GLOBAL`/`GET_GLOBAL16`/`SET_GLOBAL16` call sites the
compiler emitted, sizing the loader's `chunk->caches` array (`CacheSlot
caches[cache_count]`, `bytecode.h`) — a per-chunk, per-call-site runtime
side table holding the `Symbol*` each call site resolves to. Adding this
field to CODE's shape is the reason `PSB3_FORMAT_VERSION` bumped 1→2
(§2.1). **As of VM 2.0 Phase 2b, `cache_count` is always 0** for any freshly
compiled chunk: `GET_GLOBAL`/`SET_GLOBAL`/`GET_GLOBAL16`/`SET_GLOBAL16`, the
only opcodes that ever carried a `cache_id` operand, are themselves retired
(§3.0) — global access moved to slot-addressed `GET_GSLOT`/`SET_GSLOT`
instead (see below). The field, `chunk->caches`, and `CacheSlot` are *not*
removed: they remain live infrastructure reserved for a future Phase 8
quickening state machine (`bytecode.h`'s `CacheSlot` doc comment), just with
no current caller. Bounds: since a `cache_id` operand is `u16`, no chunk can
legitimately need more than 65536 slots; the reader rejects a `cache_count`
above that as corrupt.

**Slot-addressed globals (VM 2.0 Phase 2b, `Docs/pscal_vm2_plan.md` §5.7).**
Unlike `cache_count`, the global-slot table (`chunk->global_slots`,
`global_slot_count`, `global_slot_is_const`, `global_slot_names`, and the
three reserved-slot fields — see Chapter 1 §1.2 for what each holds) is
**not serialized at all**. It is entirely reconstructed at load time by a
new pass, `pscalLinkGlobalSlots()` (`compiler/bytecode_link.c`), that runs
on every chunk regardless of origin (fresh compile or `.bc` load) before
that chunk is verified or executed. Concretely, for the loader path
(`loadBytecodeFromCache()`/`loadBytecodeFromFile()`, `cache.c`):

```
psb3ReadChunk()            ; CODE/LINE/CONS/BMAP/PROC/TYPE sections, as before
pscalLinkGlobalSlots()     ; NEW: resolves GET_GSLOT/SET_GSLOT/GET_GSLOT_ADDRESS/
                           ; DEFINE_GLOBAL_SLOT name-index operands to slot indices,
                           ; rewriting CODE in place; builds global_slot_*
pscalVerifyBytecodeChunk() ; unchanged in spirit, but now validates the
                           ; *linked* (slot-indexed) form -- see below for why
```

**Why linking runs before verification, not after.** The compiler emits
these four opcodes with a *constant-pool name index* in the operand
position their `'s'` spec letter documents (Chapter 3) — the same position
and width the final slot index will occupy, so the link step's rewrite
never changes an instruction's length or shifts any later offset. The
verifier's job for a `'s'`-spec operand is to check `slot <
chunk->global_slot_count`; that check is meaningless before linking has
even established what `global_slot_count` is. So linking must complete
first, and by construction it only ever writes slot values in
`[0, global_slot_count)` that it assigned itself — the *only* way a
malformed `.bc` can attack this operand position is by supplying an
out-of-range *name* index before linking, which the link step itself
rejects defensively (bounds-checking against `constants_count`, since it
runs ahead of the verifier and cannot yet trust that index is in range).
This is exactly the same posture the section-directory validation in
§2.1 already takes for data consumed ahead of the verifier.

**Why this doesn't reopen the self-modifying-code question (Phase 2a,
goal G2).** The rewrite happens once, at load time, on a plain malloc'd
buffer, strictly before `pscalProtectChunkCode()` (§1.2/Chapter 4) ever
`mprotect`s it `PROT_READ` — that call happens later, in
`interpretBytecode()`'s prologue, after linking has already completed for
every path into it. It is a linker relocation pass over freshly loaded (or
freshly compiled) bytecode, not runtime self-modification of code that is
already executing.

**Fresh-compile path.** `compileASTToBytecode()` (`compiler.c`) deliberately
does **not** call `pscalLinkGlobalSlots()` before returning, even though its
output chunk needs linking before it can execute GET_GSLOT/SET_GSLOT
correctly. The reason is the cache: each frontend's `main.c` calls
`saveBytecodeToCache()` on `compileASTToBytecode()`'s direct output
immediately after compiling, and that saved `.bc` must hold the *pre-link*
(name-indexed) form — otherwise a later cache hit would run
`pscalLinkGlobalSlots()` a second time on already-linked bytecode,
misinterpreting resolved slot indices as fresh name indices (a real bug
caught during this phase's own testing: small in-range slot numbers are
frequently, but not always, valid-but-wrong constant-pool indices, so the
symptom was a confusing downstream failure, not an obvious load error).
Each frontend's `main.c` therefore calls `pscalLinkGlobalSlots()` itself,
exactly once, immediately after `saveBytecodeToCache()` and before
`interpretBytecode()` — safe to call unconditionally regardless of whether
the chunk came from a fresh compile or a cache hit, since the function is
idempotent (`chunk->globals_linked` short-circuits a second call).

**LINE** (`writeLinesSection`/`readLinesSection`): a **varint run-length
table**, replacing PSB2's per-code-byte `int[]` (4 bytes of debug info per
code byte, regardless of run length). Encoding is `[varint run_count]`
followed by `run_count × (pc_delta:uvarint, line_delta:svarint)` pairs: the
first run's `pc_delta` is its absolute pc (normally 0), the first run's
`line_delta` is its absolute line number, and every subsequent run is
relative to the previous run's `(pc, line)`. The reader expands this back
into the same flat per-code-byte `chunk->lines[]` array every other part of
the codebase already expects (the disassembler, runtime error reporting) —
only the on-disk shape changed, not the in-memory model.

**CONS** (`writeConstSection`/`readConstSection`): `[varint constants_count]`
followed by that many `Value` records via `writeValue()`/`readValue()`. Each
`Value` is tagged by its `VarType` (`u32le`) followed by a type-specific
payload, all little-endian:

| Type tag | Payload |
|----------|---------|
| `TYPE_INTEGER`/`WORD`/`BYTE`/`BOOLEAN`/`INT8`/`INT16`/`INT64` | `i_val` as `u64le` |
| `TYPE_UINT8`..`UINT64` | `u_val` as `u64le` |
| `TYPE_FLOAT` | `f32_val` as `u32le`-encoded IEEE754 bits |
| `TYPE_REAL` | `d_val` as `u64le`-encoded IEEE754 bits |
| `TYPE_LONG_DOUBLE` | truncated to `double` and stored as `u64le` IEEE754 bits (see below) |
| `TYPE_CHAR` | `c_val` as `u32le` |
| `TYPE_STRING` | presence byte (`0`/`1`; distinguishes NULL from empty), then if present a varint-length-prefixed byte string |
| `TYPE_NIL` | nothing |
| `TYPE_ENUM` | varint-length-prefixed name, then `ordinal` as `i32le` |
| `TYPE_SET` | `set_size` as `i32le`, then that many `u64le` members |
| `TYPE_ARRAY` | `dims` as `i32le`, `element_type` as `u32le`, per-dim `(lb, ub)` as `i32le` pairs, then elements recursively via `writeValue` |
| `TYPE_POINTER` | delegated to `writePointerValue`/`readPointerValue` (unchanged kinds: 0=NULL, 1=embedded `ShellCompiledFunction`, 2=owned C string, 3=opaque address) |

`TYPE_LONG_DOUBLE` is stored as a plain `double`, not the host's native
`long double` width. Every deployment target in the fleet (macOS and Linux,
both ARM64) already has `sizeof(long double) == sizeof(double)`, so this is
lossless there; on a hypothetical extended-precision x86 host it would
truncate mantissa bits beyond `double` precision. This is the accepted
trade documented in `Docs/pscal_vm2_plan.md` §9 (`TYPE_LONG_DOUBLE` is rare
in generated code per the Phase 4 note); acceptable-diff, not a bug.

Kind 1 of `writePointerValue` (an embedded `ShellCompiledFunction`, used by
exsh's compiled shell functions) recurses through the *same* six section
writers (`writeChunkCoreInline`) sequentially into the surrounding buffer,
with no directory of its own — it's never loaded standalone or randomly
addressed, so it doesn't need the top-level container's per-section
addressing. `readPointerValue()` runs `pscalLinkGlobalSlots()` then
`pscalVerifyBytecodeChunk()` (§5.5 of `Docs/pscal_vm2_plan.md`) on this
nested chunk immediately after deserializing it, exactly as the top-level
loaders do for the outer chunk — a malformed embedded closure fails the
whole load rather than surfacing as undefined behavior the first time the
closure is invoked (security fix, 2026-07-05). The link step here is
effectively a no-op walk in practice: exsh's own codegen never emits
`GET_GSLOT`/`SET_GSLOT`/`DEFINE_GLOBAL_SLOT` (shell variables go through
`CALL_HOST`/`CALL_BUILTIN` instead, being creatable at runtime), but running
it unconditionally keeps "every chunk is linked before it's verified" true
without a special case for this one nested path.

**BMAP** (`writeBmapSection`/`readBmapSection`): `[varint builtin_map_count]`,
then that many `(orig_idx:uvarint, lower_idx:uvarint)` pairs restoring
`chunk->builtin_lowercase_indices` (unchanged semantics from PSB2: pairs a
builtin-name string constant with its lowercased copy), then a `u64le`
fingerprint — an FNV-1a hash of the referenced lowercase names (plan §5.2/§9:
"ordered hash of the (name, id) pairs actually referenced by the chunk's
BMAP, not the whole registry"). **This phase computes and stores the
fingerprint but does not act on it**: an early prototype resolved each
builtin to its current numeric id at write time (`getVmBuiltinID()`) to
populate `chunk->builtin_resolved_ids` on a fingerprint match, skipping
per-call-site name resolution — but calling that resolver from
`saveBytecodeToCache()`, which runs before `interpretBytecode()` has
finished bringing up VM/extension-builtin state, was observed to corrupt
unrelated interpreter state (it surfaced as nil-valued upvalues in a Pascal
closure regression test). `chunk->builtin_resolved_ids` is therefore left
`NULL` after loading, exactly as PSB2 always left it, and vm.c's existing
per-call-site lazy resolve-by-name path runs unchanged for every loaded
chunk. A future phase can revisit the wholesale-trust optimization once the
id lookup can be made safe this early in the load path.

**PROC** (`writeProcsSection`/`readProcsSection`): `[varint proc_count]`
then `proc_count` procedure entries (recursing into nested-procedure scopes
exactly as PSB2 did, just with LE/varint primitives): varint-length-prefixed
name, `bytecode_address` (`i32le`), `locals_count` (`u16le`),
`upvalue_count` (`u8`), `type` (`u32le`), `arity` (`u8`), `has_enclosing`
(`u8`) plus a varint-length-prefixed parent name if set, then
`upvalue_count × (index:u8, isLocal:u8, is_ref:u8)`. Immediately after, in
the same section, `[varint const_sym_count]` then that many
`(name, type:u32le, value)` const-global-symbol entries. These two are
bundled into one section (rather than a standalone 8th section) since the
plan's PSB3 layout names exactly seven sections.

**TYPE** (`writeTypesSection`/`readTypesSection`): `[varint type_count]`
then that many `(name, typeAST)` pairs, where `typeAST` is a full AST
subtree via `writeAst`/`readAst` — unchanged shape from PSB2 (node type,
var type, a flags byte, token, `i_val`, left/right/extra, children), just
re-expressed with LE/varint primitives instead of raw host-native `fwrite`.

**META** (`writeMetaSection`/`readMetaSection`, cache-only): `source_hash`
and `combined_hash` as `u64le` (unchanged FNV-1a integrity hashes, computed
over the in-memory chunk — entirely unaffected by the container format
change), then a varint-length-prefixed absolute source path. Loading from
the cache reads this section first to validate the source hash and path
before trusting the rest of the file; `loadBytecodeFromFile` (explicit `.bc`
execution) never looks for `META` at all.

### 2.3 The Compilation Pipeline: `pscaljson2bc`

Unaffected by the PSB3 container change — the AST-JSON stage is the seam
between the thin frontends and the shared backend, entirely upstream of
serialization. Every frontend can emit its parsed+annotated AST as JSON
(`--dump-ast-json`, implemented by `dumpASTJSON()` in `ast.c`), and
`pscaljson2bc` — sources in the umbrella repo at `src/tools/json2bc.c` +
`src/tools/ast_json_loader.c`, linked against `pscal_core_static` — turns
that JSON back into bytecode using the same `compileASTToBytecode()` +
`finalizeBytecode()` pipeline every frontend uses, then writes it out
through the same `saveBytecodeToFile`/`psb3Write` path documented above.

```
build/bin/pascal --dump-ast-json prog.pas | build/bin/pscaljson2bc -o prog.bc
build/bin/pscalvm prog.bc
```

### 2.4 Worked Example: Source → JSON → Bytecode → Execution

Input (`add2.pas`):

```pascal
program Add2;
var a, b: integer;
begin
  a := 2;
  b := 40;
  writeln(a + b);
end.
```

Pipeline actually run against the current tree:

```
$ build/bin/pascal --dump-ast-json add2.pas > add2.json
$ build/bin/pscaljson2bc -o add2.bc add2.json               # 280-byte PSB3 file
$ build/bin/pscalvm add2.bc
42
```

Header + section directory (`xxd add2.bc`, first 96 bytes; regenerated post
VM 2.0 Phase 2b — `format_ver` is now 3, explained below):

```
00000000: 3342 5350 0300 0900 0000 0000 0600 0000  3BSP............
          └magic┘ └fmt=3┘└vm=9┘└flags──┘└count=6┘
00000010: 434f 4445 5800 0000 3300 0000 4c49 4e45  CODE offset=0x58=88 len=0x33=51  LINE
00000020: 9000 0000 0300 0000 434f 4e53 9800 0000  offset=0x90=144 len=3            CONS offset=0x98=152
00000030: 6100 0000 424d 4150 0001 0000 0b00 0000  len=0x61=97                      BMAP offset=0x100=256 len=11
00000040: 5052 4f43 1001 0000 0200 0000 5459 5045  PROC offset=0x110=272 len=2      TYPE
00000050: 1801 0000 0100 0000 ...                  offset=0x118=280 len=1
```

Six sections (no `META` — this is an explicit `.bc` file written by
`saveBytecodeToFile`, not a cache entry). `format_ver` 3 (was 2) is the VM
2.0 Phase 2b bump: no section shape changed, but the opcode retirements
below make a stale Phase-2a-vintage `.bc`'s CODE bytes structurally
valid-looking yet no longer executable — see §2.1/§2.2 for why that alone
justifies a format bump with no shape change.

Disassembly (`pscaljson2bc --dump-bytecode-only add2.json`):

```
Offset Line Opcode           Operand  Value / Target (Args)
------ ---- ---------------- -------- --------------------------
0000    0 CONSTANT            1 'nil'
0002    | DEFINE_GLOBAL_SLOT Slot:0   'myself' Type:POINTER ('')
0008    | DEFINE_GLOBAL_SLOT Slot:1   'a' Type:INTEGER ('integer')
0014    | DEFINE_GLOBAL_SLOT Slot:2   'b' Type:INTEGER ('integer')
0020    | PUSH_IMM_I8         2
0022    | GET_GSLOT_ADDRESS    1 'a'
0025    | SWAP
0026    | SET_INDIRECT
0027    | PUSH_IMM_I8        40
0029    | GET_GSLOT_ADDRESS    2 'b'
0032    | SWAP
0033    | SET_INDIRECT
0034    | CONST_1
0035    | GET_GSLOT           1 'a'
0038    | GET_GSLOT           2 'b'
0041    | ADD
0042    | CALL_BUILTIN_PROC   181 'write' (2 args)
0048    | HALT

Constants (10):
  0000: STR   "myself"    0001: NIL          0002: STR   ""
  0003: STR   "a"         0004: STR   "integer"
  0005: STR   "b"         0006: INT   2      0007: INT   40
  0008: INT   1           0009: STR   "write"
```

`DEFINE_GLOBAL_SLOT` is one byte longer per declaration than the retired
`DEFINE_GLOBAL` was for these three names (always `slot:u16` now, where
`DEFINE_GLOBAL` used a narrow `name:u8` for name indices under 256, as all
three here were): `myself`/`a`/`b` cost 6/6/6 bytes instead of 5/5/5, +3
bytes total. `GET_GSLOT` is one byte *shorter* than the retired `GET_GLOBAL`
per occurrence (`op slot:u16` = 3 bytes vs. `op name:u8 cache_id:u16` = 4),
−2 bytes for the two occurrences of `a`/`b`. `GET_GSLOT_ADDRESS` is one byte
*longer* than the retired `GET_GLOBAL_ADDRESS` per occurrence (`op slot:u16`
= 3 bytes vs. `op name:u8` = 2, since the slot form has no narrow variant —
see Chapter 3 §3.0 for why), +2 bytes for its two occurrences. Net: CODE
grows by exactly 3 bytes (48→51) for this program, matching the section
directory above. See `Docs/pscal_vm2_plan.md` §5.7 and Chapter 3 §3.0/§3.3
for the full mechanism (the load-time link step that resolves each `slot`
operand, and why the compiler emits a name index there rather than ever
choosing a slot number itself).

This particular program has no jumps, so it doesn't show off VM 2.0 Phase
1c's widened operand encodings — see Chapter 3 for the current opcode page,
where `JUMP`/`JUMP_IF_FALSE` now carry a 4-byte (`i32`) relative
displacement (was `i16`) and `CALL`/`THREAD_CREATE` now carry a 4-byte
(`u32`) code address (was `u16`), so a single chunk's code section is no
longer capped at 64KB of addressable jump/call targets. Both changes ride
this format epoch because they alter the code-stream encoding itself, not
just the container around it.
