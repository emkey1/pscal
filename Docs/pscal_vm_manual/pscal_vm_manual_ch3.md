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

- **Exactly 100 opcodes** (`0x00`–`0x63`; `OPCODE_COUNT == 100`). Opcode
  values are the enum declaration ordinals — there is no explicit `= 0xNN`
  assignment, so inserting an opcode mid-enum renumbers everything after it.
  This is why `PSCAL_VM_VERSION` (Chapter 2) exists: bytecode is only valid
  against the exact opcode numbering it was compiled for.
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
  omitted from prose (but listed in the tables).
- **Inline cache slots:** `GET_GLOBAL`/`SET_GLOBAL` (and their `*16` and
  `*_CACHED` forms) reserve `GLOBAL_INLINE_CACHE_SLOT_SIZE == 8` bytes of
  instruction stream after the name index, `_Static_assert`-checked to hold a
  `Symbol*`. The compiler emits zeros; the VM patches the resolved symbol
  pointer into the *code stream itself* on first execution. Bytecode is
  therefore self-modifying at runtime (in memory only — the cache slots are
  written as zeros to `.bc` files).

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

Globals are name-addressed (constant-pool string index), not slot-addressed —
the price of separately compiled units sharing one global table, paid down by
the 8-byte inline cache described in §3.0.

| Hex | Mnemonic | Encoding | Stack Effect | Mechanics |
|----:|----------|----------|--------------|-----------|
| 0x20 | `DEFINE_GLOBAL` | `op name:u8 type:u8 payload…` | `( init -- )` | Declare + initialize a global. Variable-length payload: for `TYPE_ARRAY`, `dims:u8` then per-dim `(lo_cidx:u16, hi_cidx:u16)` then `elem_type:u8 elem_name_cidx:u16`; for `TYPE_STRING`, `type_name_cidx:u16 len_cidx:u16` (0 = dynamic); for `TYPE_FILE`, element type info; else `type_name_cidx:u16` |
| 0x21 | `DEFINE_GLOBAL16` | `op name:u16 …` | `( init -- )` | Wide-name variant |
| 0x22 | `GET_GLOBAL` | `op name:u8 cache:8B` | `( -- v )` | Push global's value; patches resolved `Symbol*` into its inline-cache slot |
| 0x23 | `SET_GLOBAL` | `op name:u8 cache:8B` | `( v -- )` | Store into global (inline-cached) |
| 0x24 | `GET_GLOBAL_ADDRESS` | `op name:u8` | `( -- addr )` | Push pointer to the global's `Value` cell; the name `myself` resolves specially to the per-VM receiver slot `vm->threadMyself` |
| 0x25–0x27 | `GET_GLOBAL16` / `SET_GLOBAL16` / `GET_GLOBAL_ADDRESS16` | `op name:u16 [cache:8B]` | as above | Wide-name variants |
| 0x28–0x2B | `GET/SET_GLOBAL[16]_CACHED` | `op name cache:8B` | as above | Cache-required forms: identical layout, but semantically "cache already validated" — the compiler rewrites hot accessors to these |

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
0017  PUSH_IMM_I8   2        ; 07 02            — §3.1, 2 bytes
0019  GET_GLOBAL_ADDRESS 'a' ; 24 03            — §3.3 globals, 2 bytes
0021  SWAP                   ; 1E               — ( v addr -- addr v )
0022  SET_INDIRECT           ; 44               — ( addr v -- ), stores 2 into 'a'
0030  GET_GLOBAL    'a'      ; 22 03 + 8 cache bytes = 10 bytes → next at 0040
0050  ADD                    ; 08
0051  CALL_BUILTIN_PROC 181 'write' (2 args)
                             ; 51 00B5 0009 02  = 6 bytes → next at 0057
0057  HALT                   ; 59
```

Every width in that dump is predicted exactly by the encoding column of the
tables above — which is the test this chapter's tables were built to pass.
