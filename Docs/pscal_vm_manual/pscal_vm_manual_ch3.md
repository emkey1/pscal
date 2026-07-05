# PSCAL Virtual Machine Technical Manual

## Chapter 3: The Instruction Set Architecture (ISA) Reference

> Source of truth for this chapter: the `OpCode` enum in
> `components/pscal-core/src/compiler/bytecode.h:20-157` (opcode numbering),
> `disassembleInstruction()` in `src/compiler/bytecode.c:567-1349` (operand
> encodings — the disassembler's `return offset + N` values are the
> authoritative instruction widths), and the `switch` bodies in
> `src/vm/vm.c` (semantics). Handle-based JSON operations (§3.4) are builtin
> functions, not opcodes, and are documented from
> `src/ext_builtins/yyjson/yyjson_builtins.c`.

### 3.0 ISA Conventions

- **104 opcodes** (`0x00`–`0x67`; `OPCODE_COUNT == 0x68`, VM 2.0 Phase 2b —
  was 100/`0x63` through Phase 2a). Opcode values are pinned explicit
  ordinals in `opcodes.def` (not implicit enum-declaration order — that
  changed back in Phase 1a specifically so append-only opcode additions no
  longer renumber the whole page). `PSCAL_VM_VERSION` (Chapter 2) still
  gates *semantic* changes; pure ISA additions like this phase's four new
  opcodes do not require a VM-version bump (though this phase's opcode
  *retirements* did bump `PSB3_FORMAT_VERSION`, a different axis — see
  Chapter 2 §2.1).
- **Operand endianness:** multi-byte operands in the *instruction stream* are
  **big-endian** (`(code[o+1] << 8) | code[o+2]`). This is the opposite of
  the *file container* fields (Chapter 2), which are host-little-endian
  `fwrite`s. Keep the two layers straight when reading hexdumps.
- **`u8`/`u16`/`i16`/`u32`** below name operand widths. `cidx` = constant
  pool index, `slot` = frame-relative local slot number.
- **Stack effects** use Forth notation `( before -- after )`, top of stack
  rightmost. `addr` is a `Value` of `TYPE_POINTER`.
- **Wide (`*16`) variants** exist wherever a constant-pool index or count can
  exceed 255; they are semantically identical to their narrow forms and are
  omitted from prose (but listed in the tables). The Phase 2b slot-addressed
  global opcodes are the one family that does **not** follow this
  convention: they have a single always-wide (`u16`) form each, with no
  narrow variant (see below for why).
- **Global-access cache (VM 2.0 Phase 2a, `Docs/pscal_vm2_plan.md` §5.6) —
  superseded by Phase 2b, kept here for `.bc` archaeology.**
  `GET_GLOBAL`/`SET_GLOBAL`/`GET_GLOBAL16`/`SET_GLOBAL16` each carried a
  `cache_id:u16` operand after the name index, indexing a per-chunk runtime
  side table (`chunk->caches[cache_id]`, a `CacheSlot` holding the resolved
  `Symbol*`) instead of embedding the pointer in the instruction stream.
  This replaced the pre-2.0-Phase-2a design, where `GET_GLOBAL`/`SET_GLOBAL`
  reserved 8 raw bytes of instruction stream after the name index for the VM
  to self-patch a `Symbol*` into on first execution (self-modifying
  bytecode). **All of `GET_GLOBAL`/`SET_GLOBAL`/`GET_GLOBAL16`/
  `SET_GLOBAL16`/`DEFINE_GLOBAL`/`DEFINE_GLOBAL16`/`GET_GLOBAL_ADDRESS`/
  `GET_GLOBAL_ADDRESS16` (0x20-0x27) and `GET/SET_GLOBAL[16]_CACHED`
  (0x28-0x2B) are retired holes as of VM 2.0 Phase 2b**: never emitted,
  never executed (an adversarial chunk containing one gets a clean "unknown
  opcode" error), kept in `opcodes.def` only so a pre-Phase-2b standalone
  `.bc` still disassembles at its true historical mnemonic/width instead of
  "undefined opcode". `chunk->caches`/`CacheSlot`/`cache_count` are not
  removed either — they're reserved for a possible future Phase 8
  quickening state machine — but nothing populates them for a
  post-Phase-2b chunk, since no opcode carries a `cache_id` operand anymore.
- **Slot-addressed globals (VM 2.0 Phase 2b, `Docs/pscal_vm2_plan.md` §5.7):**
  `GET_GSLOT`/`SET_GSLOT`/`GET_GSLOT_ADDRESS`/`DEFINE_GLOBAL_SLOT` (0x64-0x67)
  replace the entire 0x20-0x2B family above. A `slot:u16` operand indexes
  `chunk->global_slots[]` directly — no hash lookup, no cache-miss branch,
  ever. The compiler emits a constant-pool *name* index in this operand's
  position (always `u16`, hence no narrow form: the width has to match
  whatever the eventual slot value will need, and the compiler doesn't
  choose slot numbers at all); a load-time link step
  (`compiler/bytecode_link.c`) rewrites that same 2-byte field in place into
  the resolved slot index before the chunk is verified or executed. See
  Chapter 2 §2.2 for the full link-step mechanics and Chapter 1 §1.2 for the
  runtime slot-table layout (`global_slots`/`global_slot_is_const`/
  `global_slot_names`, and the reserved-slot handling for `myself` and the
  two Pascal-exception globals). **CODE is never written after load** — in
  debug builds (`PSCAL_VM_CODE_PROTECT`) it is `mprotect(PROT_READ)`'d to
  enforce this; the link step's rewrite runs strictly before that
  `mprotect` call, so it is a load-time linker relocation, not runtime
  self-modification of already-executing code (see Chapter 2 §2.2 for why
  this distinction holds).

### 3.1 Stack & Constant Operations

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x01 | `CONSTANT` | `op cidx:u8` | `( -- v )` | Push `constants[cidx]` (deep copy for owned types) |
| 0x02 | `CONSTANT16` | `op cidx:u16` | `( -- v )` | Wide-index `CONSTANT` |
| 0x03 | `CONST_0` | `op` | `( -- 0 )` | Push integer 0 without touching the pool |
| 0x04 | `CONST_1` | `op` | `( -- 1 )` | Push integer 1 |
| 0x05 | `CONST_TRUE` | `op` | `( -- true )` | Push boolean true |
| 0x06 | `CONST_FALSE` | `op` | `( -- false )` | Push boolean false |
| 0x07 | `PUSH_IMMEDIATE_INT8` | `op imm:i8` | `( -- n )` | Push sign-extended 8-bit immediate; the compiler's choice for small integer literals, saving a pool access |
| 0x1E | `SWAP` | `op` | `( a b -- b a )` | Exchange top two values |
| 0x1F | `DUP` | `op` | `( a -- a a )` | Duplicate top of stack (deep copy for owned payloads) |
| 0x54 | `POP` | `op` | `( a -- )` | Discard top of stack, freeing owned payloads; emitted after expression statements |
| 0x5B | `FORMAT_VALUE` | `op width:u8 prec:u8` | `( v -- v' )` | Replace top with its formatted string per Pascal `:width:prec` syntax; `prec` is read back as signed (`(int8_t)`), −1 meaning "no precision" |

`CONST_0`/`CONST_1`/`CONST_TRUE`/`CONST_FALSE` are pure code-size/speed
specializations: one byte instead of two, no pool indirection. The Chapter 2
disassembly shows the compiler picking `CONST_1` for `writeln`'s implicit
argument-count value.

### 3.2 Arithmetic, Logic & Comparison

All operators in this section are single-byte, operand-free instructions —
their inputs come exclusively from the stack, and all are implemented through
the polymorphic `BINARY_OP` macro dissected in §1.1 (overload order: int32
fast path → string/char concatenation → pointer normalization → enum
arithmetic → set algebra → general numerics).

| Hex | Mnemonic | Stack Effect | Mechanics |
|----:|----------|--------------|-----------|
| 0x08 | `ADD` | `( a b -- a+b )` | Numeric add (promotes to real if either side real; integer path overflow-checked via `__builtin_add_overflow`); string/char concatenation; enum + int stepping (range-checked); set union |
| 0x09 | `SUBTRACT` | `( a b -- a−b )` | Numeric subtract; enum − int; set difference |
| 0x0A | `MULTIPLY` | `( a b -- a·b )` | Numeric multiply; set intersection |
| 0x0B | `DIVIDE` | `( a b -- a/b )` | Pascal `/`: always produces a real result, even for two integers; div-by-zero → runtime error |
| 0x15 | `INT_DIV` | `( a b -- a div b )` | Integer division (`div`); div-by-zero checked |
| 0x16 | `MOD` | `( a b -- a mod b )` | Integer modulo; div-by-zero checked |
| 0x0C | `NEGATE` | `( a -- −a )` | Unary numeric negation |
| 0x0D | `NOT` | `( a -- ¬a )` | Boolean/bitwise inversion |
| 0x0E | `TO_BOOL` | `( a -- bool )` | Coerce via truthiness rules; emitted where frontends need explicit bool contexts |
| 0x17 | `AND` | `( a b -- a∧b )` | Logical on booleans, bitwise on integers |
| 0x18 | `OR` | `( a b -- a∨b )` | Logical / bitwise or |
| 0x19 | `XOR` | `( a b -- a⊕b )` | Logical / bitwise xor |
| 0x1A | `SHL` | `( a n -- a≪n )` | Bit shift left |
| 0x1B | `SHR` | `( a n -- a≫n )` | Bit shift right |
| 0x0F | `EQUAL` | `( a b -- a=b )` | Polymorphic equality (numeric cross-type, strings, enums, sets…) |
| 0x10 | `NOT_EQUAL` | `( a b -- a≠b )` | Negated equality |
| 0x11 | `GREATER` | `( a b -- a>b )` | Ordered comparison |
| 0x12 | `GREATER_EQUAL` | `( a b -- a≥b )` | Ordered comparison |
| 0x13 | `LESS` | `( a b -- a<b )` | Ordered comparison |
| 0x14 | `LESS_EQUAL` | `( a b -- a≤b )` | Ordered comparison; doubles as subset test on sets |

Set constructors and membership live here too, since they feed the same
operators:

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x46 | `IN` | `op` | `( v set -- bool )` | Set membership test |
| 0x47 | `MAKE_SET_SINGLETON` | `op` | `( ord -- set )` | Build one-element set from an ordinal |
| 0x48 | `MAKE_SET_RANGE` | `op` | `( lo hi -- set )` | Build inclusive ordinal-range set |

### 3.3 Variables, Objects, Control Flow & Calls

#### Globals

**Globals are slot-addressed as of VM 2.0 Phase 2b** (`Docs/pscal_vm2_plan.md`
§5.7): a `slot:u16` operand indexes `chunk->global_slots[]` directly, no
hash lookup and no cache-miss branch. The compiler emits a constant-pool
*name* index in this operand's position; the load-time link step
(`compiler/bytecode_link.c`) rewrites it in place into the resolved slot
index before the chunk is verified or executed — see Chapter 2 §2.2 for
the mechanics and ordering rationale, and Chapter 1 §1.2 for the runtime
slot-table layout.

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x64 | `DEFINE_GLOBAL_SLOT` | `op slot:u16 type:u8 payload…` | `( init -- )` | Declare + initialize a global, by slot. Variable-length payload, otherwise identical in shape to the retired `DEFINE_GLOBAL16`: for `TYPE_ARRAY`, `dims:u8` then per-dim `(lo_cidx:u16, hi_cidx:u16)` then `elem_type:u8 elem_name_cidx:u16`; for `TYPE_STRING`, `type_name_cidx:u16 len_cidx:u16` (0 = dynamic); for `TYPE_FILE`, element type info; else `type_name_cidx:u16`. The bounds/type-name/elem-name/len fields are ordinary constant-pool indices, untouched by linking — only the leading `slot` field is ever rewritten from a name index |
| 0x65 | `GET_GSLOT` | `op slot:u16` | `( -- v )` | Push `chunk->global_slots[slot].symbol->value`'s contents (copied). `slot == chunk->global_myself_slot` diverts to `vm->threadMyself` instead, never touching the slot table |
| 0x66 | `SET_GSLOT` | `op slot:u16` | `( v -- )` | Store into the global at `slot`. Rejects with a runtime error if `chunk->global_slot_is_const[slot]` is set, *before* checking anything else (a const-slot write is a structural violation, not a normal control-flow skip) |
| 0x67 | `GET_GSLOT_ADDRESS` | `op slot:u16` | `( -- addr )` | Push a pointer to the global's `Value` cell (`sym->value`, stable for the chunk's whole lifetime since `global_slots[]` is sized once at link time and never reallocated). Same `myself` diversion as `GET_GSLOT` |
| 0x20–0x2B | *(retired)* | — | — | `DEFINE_GLOBAL`/`DEFINE_GLOBAL16`/`GET_GLOBAL`/`SET_GLOBAL`/`GET_GLOBAL16`/`SET_GLOBAL16`/`GET_GLOBAL_ADDRESS`/`GET_GLOBAL_ADDRESS16` (name-addressed, retired VM 2.0 Phase 2b) and `GET/SET_GLOBAL[16]_CACHED` (cache-side-table, retired VM 2.0 Phase 2a) — see §3.0 for the full retirement rationale. Never emitted or executed; kept in `opcodes.def` only so old standalone `.bc` files still disassemble with a readable legacy mnemonic and correct byte width |

#### Locals & Upvalues

Locals are slot-addressed relative to `frame->slots` (§1.2) — no name lookup
at runtime, one `u8` operand each.

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x2C | `GET_LOCAL` | `op slot:u8` | `( -- v )` | Push copy of `frame->slots[slot]` |
| 0x2D | `SET_LOCAL` | `op slot:u8` | `( v -- )` | Store into slot |
| 0x2E | `INC_LOCAL` | `op slot:u8` | `( -- )` | `slot += 1` in place; peephole-optimized `i := i + 1` |
| 0x2F | `DEC_LOCAL` | `op slot:u8` | `( -- )` | `slot -= 1` in place |
| 0x30 | `INIT_LOCAL_ARRAY` | `op slot:u8 dims:u8 (lo:u16 hi:u16)×dims elem:u8 ename:u16` | `( -- )` | Allocate and bind a local array; bounds come from pool constants |
| 0x31 | `INIT_LOCAL_FILE` | `op slot:u8 elem:u8 ename:u16` | `( -- )` | Initialize a local `file of T` variable |
| 0x32 | `INIT_LOCAL_POINTER` | `op slot:u8 tname:u16` | `( -- )` | Initialize a typed local pointer (nil) |
| 0x33 | `INIT_LOCAL_STRING` | `op slot:u8 len:u8` | `( -- )` | Initialize a fixed-length `string[len]` local |
| 0x34 | `INIT_FIELD_ARRAY` | `op field:u8 dims:u8 (lo:u16 hi:u16)×dims elem:u8 ename:u16` | `( obj -- obj )` | Like `INIT_LOCAL_ARRAY` but targets a field of the object on top of stack; used in constructors |
| 0x35 | `GET_LOCAL_ADDRESS` | `op slot:u8` | `( -- addr )` | Push pointer to the slot's `Value` cell (basis for `var` parameters) |
| 0x63 | `RESET_LOCAL` | `op slot:u8` | `( -- )` | Clear slot back to nil before reuse (scope re-entry hygiene; last opcode in the enum) |
| 0x36 | `GET_UPVALUE` | `op slot:u8` | `( -- v )` | Push value captured from an enclosing frame via `frame->upvalues[slot]` |
| 0x37 | `SET_UPVALUE` | `op slot:u8` | `( v -- )` | Store through the capture |
| 0x38 | `GET_UPVALUE_ADDRESS` | `op slot:u8` | `( -- addr )` | Address of the captured cell |

#### Records, Objects & Elements

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x4A | `ALLOC_OBJECT` | `op fields:u8` | `( -- obj )` | Allocate record/object; slot 0 is always reserved for the hidden `__vtable` pointer |
| 0x4B | `ALLOC_OBJECT16` | `op fields:u16` | `( -- obj )` | Wide field count |
| 0x4C | `GET_FIELD_OFFSET` | `op idx:u8` | `( base -- addr )` | Pop base record/pointer, push address of field `idx` (compile-time-resolved offset) |
| 0x4D | `GET_FIELD_OFFSET16` | `op idx:u16` | `( base -- addr )` | Wide offset |
| 0x4E | `LOAD_FIELD_VALUE` | `op idx:u8` | `( base -- v )` | Pop base, push copy of field value — fused `GET_FIELD_OFFSET`+`GET_INDIRECT` |
| 0x4F | `LOAD_FIELD_VALUE16` | `op idx:u16` | `( base -- v )` | Wide |
| 0x39 | `GET_FIELD_ADDRESS` | `op name:u8` | `( base -- addr )` | Field address by *name* (pool string) — used where offsets aren't statically known |
| 0x3A | `GET_FIELD_ADDRESS16` | `op name:u16` | `( base -- addr )` | Wide |
| 0x3B | `GET_FIELD_ADDRESS_KEEP` | `op name:u8` | `( base -- base addr )` | As above but keeps the base on the stack (chained access like `a.b.c := x`) |
| 0x3C | `GET_FIELD_ADDRESS_KEEP16` | `op name:u16` | `( base -- base addr )` | Wide |
| 0x3D | `LOAD_FIELD_VALUE_BY_NAME` | `op name:u8` | `( base -- v )` | Dynamic by-name field read |
| 0x3E | `LOAD_FIELD_VALUE_BY_NAME16` | `op name:u16` | `( base -- v )` | Wide |
| 0x3F | `GET_ELEMENT_ADDRESS` | `op dims:u8` | `( base i₁…iₙ -- addr )` | Pop `dims` indices then the array base; push element address (runtime bounds-checked) |
| 0x40 | `GET_ELEMENT_ADDRESS_CONST` | `op flat:u32` | `( base -- addr )` | Element address at compile-time-flattened offset — the constant-index fast path |
| 0x41 | `LOAD_ELEMENT_VALUE` | `op dims:u8` | `( base i₁…iₙ -- v )` | Fused address+load |
| 0x42 | `LOAD_ELEMENT_VALUE_CONST` | `op flat:u32` | `( base -- v )` | Fused constant-offset load |
| 0x43 | `GET_CHAR_ADDRESS` | `op` | `( straddr i -- charaddr )` | Address of character `i` in a string, tagged `STRING_CHAR_PTR_SENTINEL` (for `s[i] := 'X'`; see §1.2) |
| 0x49 | `GET_CHAR_FROM_STRING` | `op` | `( str i -- char )` | Read character `i` |
| 0x44 | `SET_INDIRECT` | `op` | `( addr v -- )` | Store `v` through pointer with type coercion — the universal assignment sink (Chapter 2's `SWAP`/`SET_INDIRECT` idiom) |
| 0x45 | `GET_INDIRECT` | `op` | `( addr -- v )` | Load through pointer |

#### Control Flow

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x1C | `JUMP_IF_FALSE` | `op off:i16` | `( cond -- )` | Pop; if falsy, `ip += off` (relative to the *end* of this 3-byte instruction) |
| 0x1D | `JUMP` | `op off:i16` | `( -- )` | Unconditional relative jump, same base |
| 0x59 | `HALT` | `op` | `( -- )` | Terminate `interpretBytecode()` for the whole chunk — top-level program exit |
| 0x5A | `EXIT` | `op` | `( -- )` | Pascal `Exit`: unwind the current routine early without halting the VM |

There is no dedicated loop/`BREAK`/`case` opcode: `while`/`for`/`repeat`,
`break`, and `case` all compile to `JUMP`/`JUMP_IF_FALSE` patterns (plus
`INC_LOCAL`/`DEC_LOCAL` for counted loops). The ±32767-byte signed offset
bounds how far a single jump can reach; the compiler is responsible for
keeping jump targets in range. Note also there are **no contract-check
opcodes** — no `@pre`/`@post` instruction exists in this ISA; assertion-like
semantics are frontend lowerings onto `JUMP_IF_FALSE` + builtin error calls.

#### Calls & Returns

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x55 | `CALL` | `op name:u16 addr:u16 argc:u8` | `( args… -- ret )` | Direct call: push a `CallFrame` (return address, `slots` window over the `argc` args), jump to `addr`. `name` is for diagnostics/frame metadata |
| 0x52 | `CALL_USER_PROC` | `op name:u16 argc:u8` | `( args… -- ret )` | Late-bound call: target address resolved by name through `procedureTable` at call time (separately compiled / forward refs) |
| 0x56 | `CALL_INDIRECT` | `op argc:u8` | `( args… fnptr -- ret )` | Call through a function-pointer/closure value on the stack |
| 0x58 | `PROC_CALL_INDIRECT` | `op argc:u8` | `( args… fnptr -- )` | Statement-context indirect call; sets `discard_result_on_return` so any result is dropped |
| 0x57 | `CALL_METHOD` | `op midx:u8 argc:u8` | `( self args… -- ret )` | Virtual dispatch: fetch method `midx` from the receiver's `__vtable` (object slot 0), then call; `frame->vtable` is set for nested dispatch |
| 0x50 | `CALL_BUILTIN` | `op name:u16 argc:u8` | `( args… -- ret )` | Invoke a registered builtin *function* by name-constant (case-normalized via the Chapter 2 lowercase map) |
| 0x51 | `CALL_BUILTIN_PROC` | `op id:u16 name:u16 argc:u8` | `( args… -- )` | Invoke a builtin *procedure* by pre-resolved numeric `id` (name kept for diagnostics) — the fastest builtin path, no lookup |
| 0x53 | `CALL_HOST` | `op id:u8` | `( … -- v )` | Invoke a `HostFunctionID` from `vm->host_functions[]` (`HOST_FN_CREATE_CLOSURE`, `HOST_FN_INTERFACE_LOOKUP`, shell loop hooks…) — the VM↔frontend-runtime seam, distinct from user-visible builtins |
| 0x00 | `RETURN` | `op` | `( ret -- )` in callee | Pop the current `CallFrame`, restore `ip = return_address`, hand the result to the caller (or drop it if `discard_result_on_return`) |

### 3.4 Opaque Data & Handle Semantics (yyjson)

**There are no JSON opcodes.** Handle-based structured data is implemented
entirely as builtins reached through `CALL_BUILTIN` — the ISA's opaque-data
story is "integers on the stack, meaning in a native-side table." (The
original design brief's "TOON handles" do not exist under that name; this is
the real mechanism.)

The handle table (`yyjson_builtins.c:11-29`):

```c
typedef enum { JSON_HANDLE_UNUSED = 0, JSON_HANDLE_DOC, JSON_HANDLE_VAL } JsonHandleKind;

typedef struct {
    JsonHandleKind kind;
    yyjson_doc *doc;     // owned document (DOC handles)
    yyjson_val *val;     // borrowed value within a document (VAL handles)
    size_t refcount;
    int doc_handle;      // VAL -> parent DOC back-reference
} JsonHandleEntry;

static JsonHandleEntry *jsonHandleTable;      // global, grows on demand
static pthread_mutex_t jsonHandleMutex;       // thread-safe across VM threads
```

A handle is a plain PSCAL integer indexing this table — copyable, storable in
variables and arrays, passable between threads. The `VAL`→`DOC`
`doc_handle` back-reference plus refcounting keeps a parsed document alive as
long as any value handle into it survives; frontends must still explicitly
free (`YyjsonFreeValue`, `YyjsonDocFree`) since handles are not
garbage-collected — a leaked handle pins the whole parsed document.

Builtin surface (registered names; case-insensitive at the call site):

| Builtin | Signature (conceptual) | Returns |
|---------|------------------------|---------|
| `YyjsonRead` | `(jsonText: string)` | DOC handle (−1 on parse error) |
| `YyjsonReadFile` | `(path: string)` | DOC handle |
| `YyjsonGetRoot` | `(doc: int)` | VAL handle of root |
| `YyjsonGetKey` | `(val: int; key: string)` | VAL handle of object member |
| `YyjsonGetIndex` | `(val: int; i: int)` | VAL handle of array element |
| `YyjsonHasKey` / `YyjsonHasIndex` | `(val, key/i)` | boolean |
| `YyjsonGetLength` | `(val: int)` | element/member count |
| `YyjsonGetType` | `(val: int)` | type tag |
| `YyjsonGetString` / `GetNumber` / `GetInt` / `GetBool` | `(val: int)` | scalar payload |
| `YyjsonIsNull` | `(val: int)` | boolean |
| `YyjsonFreeValue` | `(val: int)` | decref VAL (and parent DOC) |
| `YyjsonDocFree` | `(doc: int)` | decref/free document |

At the ISA level, `n := YyjsonGetInt(v)` is nothing more exotic than:

```
GET_LOCAL            2 (slot)          ; push handle integer v
CALL_BUILTIN       147 'yyjsongetint' (1 args)
SET_LOCAL            3 (slot)          ; store result
```

The same pattern carries every other native handle domain — HTTP sessions,
sockets, threads-by-id, mutexes — with one exception: threads and mutexes
*do* get dedicated opcodes (§3.5) because they need interpreter-loop
cooperation (frame setup, blocking semantics) that a builtin call can't
express as cleanly.

### 3.5 Threading & Synchronization Opcodes

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x5C | `THREAD_CREATE` | `op entry:u16` | `( -- tid )` | Spawn a worker: claim a `Thread` slot (§1.4), give it its own `VM` (own stack/frames, shared globals), start `interpretBytecode()` at bytecode offset `entry`; push the thread id |
| 0x5D | `THREAD_JOIN` | `op` | `( tid -- result )` | Block on the worker's `resultCond` until `resultReady`; push the thread's result value |
| 0x5E | `MUTEX_CREATE` | `op` | `( -- mid )` | Allocate a slot in `vm->mutexes[]` (max 64), init a `pthread_mutex_t`; push its id |
| 0x5F | `RCMUTEX_CREATE` | `op` | `( -- mid )` | As above with `PTHREAD_MUTEX_RECURSIVE` |
| 0x60 | `MUTEX_LOCK` | `op` | `( mid -- )` | Block until the mutex is acquired |
| 0x61 | `MUTEX_UNLOCK` | `op` | `( mid -- )` | Release |
| 0x62 | `MUTEX_DESTROY` | `op` | `( mid -- )` | Destroy and free the slot for reuse |

`THREAD_CREATE`'s `u16` entry operand mirrors `interpretBytecode()`'s
`uint16_t entry` parameter (§1.1): a spawned thread's entry point must lie in
the first 64 KiB of the chunk. Cooperative pause/cancel/kill (§1.4) has no
opcodes — it is host-API-driven (`vmThreadPause` etc.), with the interpreter
checking the atomics at safe points.

### 3.6 Reading a Real Instruction Stream

Tying the tables back to Chapter 2's worked example, byte by byte:

```
0017  PUSH_IMM_I8   2         ; 07 02            — §3.1, 2 bytes
0019  GET_GSLOT_ADDRESS 3 'a' ; 67 0003          — §3.3 globals, slot:u16 = 3 bytes
0022  SWAP                    ; 1E               — ( v addr -- addr v )
0023  SET_INDIRECT            ; 44               — ( addr v -- ), stores 2 into 'a'
0030  GET_GSLOT      3 'a'    ; 65 0003          — slot:u16 = 3 bytes → next at 0033
0033  ADD                     ; 08
0034  CALL_BUILTIN_PROC 181 'write' (2 args)
                              ; 51 00B5 0009 02  = 6 bytes → next at 0040
0040  HALT                    ; 59
```

(`GET_GSLOT`/`GET_GSLOT_ADDRESS` print the slot's name — `'a'` — purely for
readability; the disassembler resolves it from `chunk->global_slot_names[slot]`,
never from the constant pool, since the operand is a slot index, not a
constant-pool index. See Chapter 2 §2.2.)

Every width in that dump is predicted exactly by the encoding column of the
tables above — which is the test this chapter's tables were built to pass.
