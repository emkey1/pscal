# Pscal Virtual Machine Documentation

### **Pscal VM Architecture**

The Pscal VM is a **stack-based virtual machine**. This means that it uses a stack data structure to store and manipulate data during program execution. Instead of operating on registers like a physical CPU, most instructions (opcodes) operate on values at the top of the stack.

#### **Core Components**

The VM's architecture is defined by the `VM` struct in `src/vm/vm.h` and consists of the following key components:

* **Instruction Pointer (IP):** A pointer (`ip`) that always points to the *next* bytecode instruction to be executed.
* **Last Instruction:** A pointer (`lastInstruction`) to the start of the last instruction that was executed, used for diagnostics and error reporting.
* **Operand Stack:** A fixed-size array (`stack`, max `VM_STACK_MAX = 8192` entries) that holds `Value` structs. `Value` is a versatile struct that can represent all of Pscal's data types, including integers, reals, strings, pointers, and more.
    * **`stackTop`:** A pointer to the next available slot on the stack. When a value is pushed, it's placed where `stackTop` points, and then `stackTop` is incremented.
* **Call Stack (Frames):** An array of `CallFrame` structs (`frames`, max `VM_CALL_STACK_MAX = 4096`) that manages function and procedure calls. Each time a function is called, a new `CallFrame` is pushed onto this stack. A `CallFrame` contains:
    * **`return_address`**: The IP in the caller to return to when the function finishes.
    * **`slots`**: A pointer to the beginning of the current function's section on the main operand stack. This marks the start of its parameters and local variables.
    * **`function_symbol`**: A pointer to the `Symbol` table entry for the function being called, which provides metadata like the number of local variables and upvalues.
    * **`slotCount`**: Total slots (arguments + locals) reserved for this frame.
    * **`locals_count`**: Number of local variables (excluding parameters).
    * **`upvalue_count`**: Number of captured upvalues from enclosing scopes.
    * **`upvalues`**: Array of pointers to captured variable locations.
    * **`owns_upvalues`**: Whether this frame is responsible for freeing the upvalue array.
    * **`closureEnv`**: Pointer to the captured closure environment payload.
    * **`discard_result_on_return`**: If `true`, the return value is dropped on return (used when a function call appears in statement context).
    * **`vtable`**: Reference to the class V-table when executing a method.
* **Symbol Tables:** The VM maintains pointers to three hash tables:
    * **`vmGlobalSymbols`:** A `HashTable` for runtime storage and lookup of global variables.
    * **`vmConstGlobalSymbols`:** A separate `HashTable` for constant globals (read-only, no mutex). These are compiled as `const` declarations and cannot be modified at runtime.
    * **`procedureTable`:** A `HashTable` that stores information about all compiled procedures and functions, which is used for disassembly and resolving calls.
* **Bytecode Chunk:** A pointer (`chunk`) to the `BytecodeChunk` being executed. This chunk contains a version field (`version`), the bytecode instructions (`code`), the constant pool (`constants`), line number information for debugging (`lines`), a builtin lowercase index mapping, and a global symbol cache for inline cached accesses.
* **Host Functions:** An array (`host_functions`, max `MAX_HOST_FUNCTIONS = 4096`) of C function pointers registered with the VM via `CALL_HOST`.
* **Thread Table:** The VM spawns real OS-level threads via `pthread`. Each entry in the `threads` array (max `VM_MAX_THREADS = 16`) holds a native thread and its own `VM` instance, allowing bytecode routines to execute in parallel.
* **Mutex Table:** Runtime-created mutex objects live in the `mutexes` array (max `VM_MAX_MUTEXES = 64`). Each slot tracks a native `pthread_mutex_t` along with whether it is active, enabling synchronization between threads.
* **Execution Control Flags:**
    * **`exit_requested`**: Set by a builtin to request early exit from the current frame (similar to `EXIT` but triggerable from C code).
    * **`abort_requested`**: Raised when a builtin requests an immediate interpreter abort (e.g., unrecoverable error).
    * **`suspend_unwind_requested`**: Set for cooperative Ctrl-Z style suspension; the VM continues unwinding frames rather than halting immediately.
* **Frontend Context:** A `void*` pointer (`frontendContext`) allowing frontends (e.g., exsh) to attach per-VM state.
* **String Indexing Mode:** A boolean (`shellIndexing`) that controls whether string indexing is 0-based (shell style) or 1-based (Pascal/REA style).

#### **Worker Thread Pool & Lifecycle**

The thread table doubles as a **reusable worker pool**. Worker slots are lazily created up to `VM_MAX_WORKERS` and parked in an internal `ThreadJobQueue` (`jobQueue`) when idle so new jobs reuse existing OS threads before spawning more. The VM tracks `workerCount`, `availableWorkers`, and `shuttingDownWorkers` for pool management. Registry access is protected by `threadRegistryLock`. Each `Thread` now tracks:

* **Identity:** `name` (set via `vmThreadAssignName`/`vmThreadFindIdByName`) and a `poolGeneration` counter so reused slots remain distinct to debuggers.
* **Lifecycle flags:** cooperative `paused`, `cancelRequested`, and `killRequested` atomics, plus `awaitingReuse`/`readyForReuse` to gate when the pool may reclaim a worker. Callers may pause/resume/cancel/kill workers through `vmThreadPause`, `vmThreadResume`, `vmThreadCancel`, and `vmThreadKill`.
* **Timing & metrics:** wall-clock timestamps (`queuedAt`, `startedAt`, `finishedAt`) together with cached CPU/RSS samples stored in `ThreadMetrics`. The helper `vmSnapshotWorkerUsage` gathers non-blocking snapshots for diagnostic tooling.
* **Worker ownership:** Threads running inside worker slots have `owningThread` and `threadId` set to identify their pool context.

Jobs publish both a result payload and a success flag; a worker is only released back to the queue once *both* have been consumed. Use `ThreadGetResult(t, /*consumeStatus=*/true)` to clear the result and status in a single call, or pair `ThreadGetResult` with `ThreadGetStatus(t, /*dropResult=*/true)` when you prefer to stream the value first.

#### **Builtin registry and procedure cache**

Dispatch-heavy paths in the VM lean on two caches:

* The builtin registry is backed by a canonical-name hash table so `registerVmBuiltin`
  and runtime lookups resolve handlers in constant time rather than walking the
  dispatch arrays.
* Procedure symbols discovered during compilation are copied into a dense
  `procedureByAddress` array the first time a chunk is loaded. Subsequent
  invocations resolve call targets directly from that cache, falling back to the
  full symbol table only when the entry is missing.

Both caches are cleared or rebuilt automatically when new bytecode is installed,
keeping lookups fast while ensuring that hot swaps still honour updated
definitions.

#### **Inline Global Caches**

Hot global variable accesses can use inline cached variants (`*_CACHED` opcodes). These embed a `Symbol*` pointer directly in the bytecode after the opcode, stored in a slot of `GLOBAL_INLINE_CACHE_SLOT_SIZE` bytes. On first execution, the cache is populated; subsequent executions skip the symbol table lookup entirely. If the cached symbol is invalid, a runtime error is raised.

#### **Execution Flow**

The VM's execution is driven by the `interpretBytecode` function in `src/vm/vm.c`. This function contains a main loop that repeatedly performs the following steps:

1.  Reads the next bytecode instruction pointed to by `ip`.
2.  Records `lastInstruction = ip` for diagnostics.
3.  Decodes the instruction and its operands.
4.  Performs the operation defined by the instruction, which typically involves pushing, popping, or manipulating values on the stack.
5.  Updates the instruction pointer to the next instruction.
6.  This loop continues until it encounters a `HALT` instruction or a `RETURN` from the main program body (or an `abort_requested` flag from a host function).

Optional tracing can be enabled by setting `trace_head_instructions` to a positive value, which causes the VM to print the first N instructions as they execute.

---

### **Opcode Reference**

The following is a complete list of opcodes supported by the Pscal VM, as defined in `src/compiler/bytecode.h`, with integrated examples of their usage.

#### **Immediate and Constant Opcodes**

* **`CONSTANT`**:
    * **Operands:** 1-byte index into the constant pool.
    * **Action:** Pushes the constant value at the specified index onto the stack.
* **`CONSTANT16`**:
    * **Operands:** 2-byte index into the constant pool.
    * **Action:** Same as `CONSTANT`, but for constant pools with more than 256 entries.
* **`CONST_0`**:
    * **Operands:** None.
    * **Action:** Pushes the integer `0` directly onto the stack without a constant pool lookup. Peephole optimization for common zero literals.
* **`CONST_1`**:
    * **Operands:** None.
    * **Action:** Pushes the integer `1` directly onto the stack. Peephole optimization for common one literals.
* **`CONST_TRUE`**:
    * **Operands:** None.
    * **Action:** Pushes `boolean true` onto the stack.
* **`CONST_FALSE`**:
    * **Operands:** None.
    * **Action:** Pushes `boolean false` onto the stack.
* **`PUSH_IMMEDIATE_INT8`**:
    * **Operands:** 1-byte signed integer (two's complement; values 0x00–0x7F = 0–127, 0x80–0xFF = −128–−1).
    * **Action:** Pushes a small integer directly onto the stack without using the constant pool. Useful for frequently used small constants and peephole optimization.

#### **Stack Manipulation Opcodes**

* **`POP`**:
    * **Operands:** None.
    * **Action:** Pops the top value from the stack and discards it.
* **`SWAP`**:
    * **Operands:** None.
    * **Action:** Swaps the top two values on the stack.
* **`DUP`**:
    * **Operands:** None.
    * **Action:** Duplicates the top value on the stack.

#### **Arithmetic and Logical Opcodes**

* **`ADD`**, **`SUBTRACT`**, **`MULTIPLY`**, **`DIVIDE`**, **`INT_DIV`**, **`MOD`**:
    * **Operands:** None.
    * **Action:** Pop two values from the stack, perform the specified arithmetic operation, and push the result. `DIVIDE` always produces a real result, while `INT_DIV` performs integer division. `ADD` is also overloaded for string concatenation.
* **`NEGATE`**:
    * **Operands:** None.
    * **Action:** Pops one value, negates it, and pushes the result.
* **`NOT`**:
    * **Operands:** None.
    * **Action:** Pops one boolean value, inverts it, and pushes the result.
* **`TO_BOOL`**:
    * **Operands:** None.
    * **Action:** Pops one value, applies the VM's truthiness rules (numbers, characters, and booleans), and pushes the resulting boolean without inversion.
* **`AND`**, **`OR`**, **`XOR`**, **`SHL`**, **`SHR`**:
    * **Operands:** None.
    * **Action:** Pop two integer values, perform the specified bitwise operation, and push the result. `AND`, `OR`, and `XOR` also support logical operations on booleans.
* **`EQUAL`**, **`NOT_EQUAL`**, **`GREATER`**, **`GREATER_EQUAL`**, **`LESS`**, **`LESS_EQUAL`**:
    * **Operands:** None.
    * **Action:** Pop two values, perform the specified comparison, and push the boolean result (`true` or `false`).

---
**Example: Arithmetic and Assignment**

Consider the following line of Pascal code:

```pascal
a := 5 + 3;
```

The compiler would translate this into the following sequence of bytecode instructions:

1.  `CONSTANT <index_of_5>`
    * **Action:** The VM pushes the integer value `5` from the constant pool onto the stack.
    * **Stack:** `[5]`
2.  `CONSTANT <index_of_3>`
    * **Action:** The VM pushes the integer value `3` from the constant pool onto the stack.
    * **Stack:** `[5, 3]`
3.  `ADD`
    * **Action:** The VM pops the top two values (`3` and `5`), adds them together, and pushes the result (`8`) back onto the stack.
    * **Stack:** `[8]`
4.  `SET_GLOBAL <index_of_a>`
    * **Action:** The VM pops the result (`8`) from the stack and stores it in the global variable `a`.
    * **Stack:** `[]`

With peephole optimization enabled, the compiler may instead emit:

1.  `PUSH_IMMEDIATE_INT8 5` — or `CONST_0`/`CONST_1` for 0/1
2.  `PUSH_IMMEDIATE_INT8 3`
3.  `ADD`
4.  `SET_GLOBAL <index_of_a>`

---

#### **Control Flow Opcodes**

* **`JUMP`**:
    * **Operands:** 2-byte signed offset.
    * **Action:** Unconditionally jumps the instruction pointer by the specified offset.
* **`JUMP_IF_FALSE`**:
    * **Operands:** 2-byte signed offset.
    * **Action:** Pops a value from the stack. If the value is `false` (or numerically zero), jumps the instruction pointer by the specified offset.

---
**Example: Conditional Logic**

Here's how an `if` statement would be compiled:

```pascal
if a > b then
  c := 10;
```

1.  `GET_GLOBAL <index_of_a>`
    * **Action:** Push the value of `a`.
    * **Stack:** `[8]`
2.  `GET_GLOBAL <index_of_b>`
    * **Action:** Push the value of `b`.
    * **Stack:** `[8, 8]`
3.  `GREATER`
    * **Action:** Pop `8` and `8`, compare them (`8 > 8` is false), and push the boolean result.
    * **Stack:** `[false]`
4.  `JUMP_IF_FALSE <offset>`
    * **Action:** Pop the boolean value. Since it's `false`, the VM jumps the instruction pointer forward by the specified offset, skipping the code for the `then` block.
    * **Stack:** `[]`

If `a` had been greater than `b`, the `JUMP_IF_FALSE` would not have jumped, and the code to assign `10` to `c` would have been executed.

---

**Example: While Loop**

```pascal
i := 0;
while i < 3 do
begin
  writeln(i);
  i := i + 1;
end;
```

1. `CONSTANT <index_of_0>`
   * Push integer `0`.
2. `SET_GLOBAL <index_of_i>`
   * Store in variable `i`.
3. loop_start:
   * `GET_GLOBAL <index_of_i>`
   * `CONSTANT <index_of_3>`
   * `LESS`
   * `JUMP_IF_FALSE <exit_offset>`
4. loop_body:
   * `CONSTANT <index_of_true>` – newline flag
   * `GET_GLOBAL <index_of_i>`
   * `CALL_BUILTIN <index_of_write> 2`
   * `GET_GLOBAL <index_of_i>`
   * `CONSTANT <index_of_1>`
   * `ADD`
   * `SET_GLOBAL <index_of_i>`
   * `JUMP <loop_start>`
5. exit:
   * (next instruction after loop)

With peephole optimization, `i := i + 1` can be replaced by `INC_LOCAL` (or the equivalent global increment path), and `CONSTANT <index_of_0>` / `CONSTANT <index_of_3>` may become `CONST_0` / `PUSH_IMMEDIATE_INT8 3`.

This sequence uses `JUMP_IF_FALSE` to exit the loop and `JUMP` to repeat.

---

#### **Global Variable Opcodes**

* **`DEFINE_GLOBAL`** / **`DEFINE_GLOBAL16`**:
    * **Operands:** Variable-length. Includes a constant index for the variable's name (8-bit for `DEFINE_GLOBAL`, 16-bit for `DEFINE_GLOBAL16`), the variable's type, and additional type information (e.g., array dimensions, record structure).
    * **Encoding:** `[name_const_idx][var_type_enum][payload...]`. For `TYPE_ARRAY`: `[dim_count] { [lower_idx][upper_idx] }*dim_count [elem_var_type][elem_type_name_idx]`. For `TYPE_STRING`: includes a `[len_const_idx]` (0 = dynamic length).
    * **Action:** Defines a new global variable in the VM's global symbol table.
* **`GET_GLOBAL`** / **`GET_GLOBAL16`**:
    * **Operands:** 8-bit or 16-bit constant index for the variable's name.
    * **Action:** Pushes the value of the specified global variable onto the stack.
* **`SET_GLOBAL`** / **`SET_GLOBAL16`**:
    * **Operands:** 8-bit or 16-bit constant index for the variable's name.
    * **Action:** Pops a value from the stack and assigns it to the specified global variable.
* **`GET_GLOBAL_ADDRESS`** / **`GET_GLOBAL_ADDRESS16`**:
    * **Operands:** 8-bit or 16-bit constant index for the variable's name.
    * **Action:** Pushes a pointer to the specified global variable's `Value` struct onto the stack.
* **`GET_GLOBAL_CACHED`** / **`GET_GLOBAL16_CACHED`**:
    * **Operands:** 1-byte or 2-byte constant index (read but unused after first call) + `GLOBAL_INLINE_CACHE_SLOT_SIZE` (8) bytes of inline `Symbol*` cache.
    * **Action:** Same as `GET_GLOBAL`/`GET_GLOBAL16`, but uses an embedded inline cache for fast repeated access. On first execution, resolves the symbol and writes the `Symbol*` pointer into the bytecode stream; subsequent executions read directly from the cache. Raises a runtime error if the cached symbol becomes invalid.
* **`SET_GLOBAL_CACHED`** / **`SET_GLOBAL16_CACHED`**:
    * **Operands:** Same as `GET_GLOBAL_CACHED`/`GET_GLOBAL16_CACHED`.
    * **Action:** Same as `SET_GLOBAL`/`SET_GLOBAL16`, but with inline `Symbol*` cache. Requires the cache to already be populated (typically by a preceding `GET_GLOBAL_CACHED` to the same global).

#### **Local Variable Opcodes**

* **`GET_LOCAL`** / **`SET_LOCAL`**:
    * **Operands:** 1-byte slot index within the current call frame.
    * **Action:** `GET_LOCAL` pushes the value of the local variable at the given slot. `SET_LOCAL` pops a value and assigns it to the local variable.
* **`GET_LOCAL_ADDRESS`**:
    * **Operands:** 1-byte slot index.
    * **Action:** Pushes a pointer to the specified local variable's `Value` struct onto the stack.
* **`INC_LOCAL`**:
    * **Operands:** 1-byte slot index.
    * **Action:** Peephole optimization helper. Increments the value at the specified local slot by 1. Validates the slot is within the current frame's window.
* **`DEC_LOCAL`**:
    * **Operands:** 1-byte slot index.
    * **Action:** Peephole optimization helper. Decrements the value at the specified local slot by 1. Validates the slot is within the current frame's window.
* **`RESET_LOCAL`**:
    * **Operands:** 1-byte slot index.
    * **Action:** Clears the value at the specified local slot back to `nil`. Used to release resources held by a variable before the slot is reused (e.g., in loop bodies where a local may hold a large string or object from a previous iteration).

#### **Upvalue (Closure) Opcodes**

* **`GET_UPVALUE`** / **`SET_UPVALUE`**:
    * **Operands:** 1-byte upvalue index.
    * **Action:** Accesses a variable from an enclosing function's scope (a "closure"). `GET_UPVALUE` pushes the value, and `SET_UPVALUE` assigns to it.
* **`GET_UPVALUE_ADDRESS`**:
    * **Operands:** 1-byte upvalue index.
    * **Action:** Pushes a pointer to the specified upvalue's `Value` struct.

#### **Initialization Opcodes**

* **`INIT_LOCAL_ARRAY`**:
    * **Operands:** Variable-length, including a slot index and type metadata.
    * **Action:** Initializes a local array variable at the specified slot. For arrays, any dimension using the sentinel bound index `0xFFFF` will pop its size from the stack (treated as an upper bound plus one) and assume a lower bound of `0`.
* **`INIT_LOCAL_FILE`**:
    * **Operands:** 1-byte slot index.
    * **Action:** Initializes a local file variable at the specified slot.
* **`INIT_LOCAL_POINTER`**:
    * **Operands:** Variable-length, including a slot index and type metadata.
    * **Action:** Initializes a local pointer variable at the specified slot.
* **`INIT_LOCAL_STRING`**:
    * **Operands:** 1-byte slot index, 1-byte fixed length.
    * **Action:** Initializes a local fixed-length string variable at the specified slot with the given maximum length. Allocates a buffer of `length + 1` bytes (null-terminated). Used for Pascal-style `string[N]` declarations.
* **`INIT_FIELD_ARRAY`**:
    * **Operands:** 1-byte field index, 1-byte dimension count, then pairs of `[lower_idx][upper_idx]` per dimension.
    * **Action:** Initializes an array field within an existing object/record. Similar to `INIT_LOCAL_ARRAY` but targets a specific field by index rather than a local slot.

#### **Record and Object Opcodes**

* **`ALLOC_OBJECT`** / **`ALLOC_OBJECT16`**:
    * **Operands:** 1-byte (or 2-byte for `ALLOC_OBJECT16`) field count.
    * **Action:** Allocates a new record/object on the heap with the given number of fields. The first field slot is always reserved for a hidden `__vtable` pointer (used by `CALL_METHOD`). All fields are initialized to `nil`.
* **`GET_FIELD_ADDRESS`** / **`GET_FIELD_ADDRESS16`**:
    * **Operands:** Constant index for the field's name (1-byte or 2-byte).
    * **Action:** Pops a record or a pointer to a record from the stack and pushes a pointer to the specified field's `Value` struct. Consumes the base value from the stack.
* **`GET_FIELD_ADDRESS_KEEP`** / **`GET_FIELD_ADDRESS_KEEP16`**:
    * **Operands:** Constant index for the field's name (1-byte or 2-byte).
    * **Action:** Same as `GET_FIELD_ADDRESS`, but does *not* consume the base record/pointer from the stack. Used for chained field access (e.g., `a.b.c` where `a.b` must remain for the next access).
* **`GET_FIELD_OFFSET`** / **`GET_FIELD_OFFSET16`**:
    * **Operands:** 1-byte (or 2-byte) zero-based field index.
    * **Action:** Pops a base pointer/record from the stack and pushes the address of the field at the given numeric offset. Unlike `GET_FIELD_ADDRESS`, this uses a numeric offset rather than a name lookup, making it faster when the offset is known at compile time.
* **`LOAD_FIELD_VALUE`** / **`LOAD_FIELD_VALUE16`**:
    * **Operands:** 1-byte (or 2-byte) field offset.
    * **Action:** Pops a record or pointer to a record (including chained pointers), resolves the field by offset—respecting hidden vtable slots—and pushes a copy of the field's value onto the stack.
* **`LOAD_FIELD_VALUE_BY_NAME`** / **`LOAD_FIELD_VALUE_BY_NAME16`**:
    * **Operands:** Constant index of the field name (1-byte or 2-byte).
    * **Action:** Pops a record or pointer to a record, locates the named field, and pushes a copy of its value. Emits a runtime error if the field does not exist.

#### **Array and String Element Opcodes**

* **`GET_ELEMENT_ADDRESS`**:
    * **Operands:** 1-byte dimension count.
    * **Action:** Pops an array or pointer to an array, and then pops the indices for each dimension. Pushes a pointer to the specified element's `Value` struct.
* **`GET_ELEMENT_ADDRESS_CONST`**:
    * **Operands:** 4-byte flat element offset.
    * **Action:** Pops an array or pointer to an array and pushes the address of the element at the precomputed flat offset. Bounds must have been validated by the compiler when emitting the instruction.
* **`LOAD_ELEMENT_VALUE`**:
    * **Operands:** 1-byte dimension count.
    * **Action:** Pops an array (or pointer to an array) and the indices for each dimension, checks bounds, and pushes a copy of the addressed element's value. Handles Pascal strings specially so that `s[0]` yields the length and `s[i]` yields the character value.
* **`LOAD_ELEMENT_VALUE_CONST`**:
    * **Operands:** 4-byte flat element offset.
    * **Action:** Pops an array (or pointer to an array) and pushes a copy of the element at the provided constant flat offset. Intended for accesses whose indices were folded at compile time.
* **`GET_CHAR_ADDRESS`**:
    * **Operands:** None.
    * **Action:** Pops an index and a pointer to a string. Pushes a pointer to the character at that index within the string.
* **`GET_CHAR_FROM_STRING`**:
    * **Operands:** None.
    * **Action:** Pops an index and a string. Pushes the character at that index as a value.

#### **Pointer and Indirect Access Opcodes**

* **`SET_INDIRECT`**:
    * **Operands:** None.
    * **Action:** Pops a value and a pointer. Assigns the value to the memory location indicated by the pointer.
* **`GET_INDIRECT`**:
    * **Operands:** None.
    * **Action:** Pops a pointer and pushes a copy of the value it points to.

#### **Set Membership**

* **`IN`**:
    * **Operands:** None.
    * **Action:** Pops an item and a set. Pushes `true` if the item is in the set, `false` otherwise.

---
**Example: Record Field Assignment**

Consider the Pascal snippet:

```pascal
var
  p: TPoint;
begin
  p.x := 10;
end.
```

Bytecode emitted:

1. `GET_GLOBAL_ADDRESS <index_of_p>`
   * Push a pointer to the global variable `p`.
2. `GET_FIELD_ADDRESS <index_of_x>`
   * Pop the record pointer, push a pointer to field `x`.
3. `CONSTANT <index_of_10>`
   * Push the integer constant `10`.
4. `SET_INDIRECT`
   * Pop value and pointer; store `10` in `p.x`.

---

#### **Function and Procedure Call Opcodes**

* **`CALL`**:
    * **Operands:** 2-byte name index (for disassembly), 2-byte bytecode address, 1-byte argument count.
    * **Action:** Calls a user-defined function or procedure at the specified address. The name index is read and discarded at runtime (used only by the disassembler). Creates a new `CallFrame` and jumps to the target address.
* **`CALL_USER_PROC`**:
    * **Operands:** 2-byte name constant index, 1-byte argument count.
    * **Action:** Calls a user-defined procedure/function by name (resolves the target address from the procedure table). Unlike `CALL`, does not encode the address directly — used by frontends that prefer late binding via the symbol table.
* **`CALL_INDIRECT`**:
    * **Operands:** 1-byte argument count.
    * **Action:** Pops an address value from the stack (a closure or raw offset), then calls it with the top `arg_count` values as arguments. Supports closures — if the address is of type `TYPE_CLOSURE`, the captured environment is retained for the duration of the call.
* **`PROC_CALL_INDIRECT`**:
    * **Operands:** 1-byte argument count.
    * **Action:** Same as `CALL_INDIRECT`, but runs in statement context — any return value from the callee is discarded. Used for calls to function pointers in procedure position.
* **`CALL_METHOD`**:
    * **Operands:** 1-byte method index, 1-byte argument count.
    * **Action:** Virtual method dispatch. The receiver is expected at `stackTop[-arg_count - 1]`. Resolves the method through the object's hidden `__vtable` pointer using the method index. The receiver must be a non-nil pointer to a `TYPE_RECORD`.
* **`CALL_BUILTIN`**:
    * **Operands:** 2-byte name constant index, 1-byte argument count.
    * **Action:** Calls a built-in function or procedure by name. Looks up the name in the constant pool, then dispatches to the registered builtin handler. The return value (if any) is pushed onto the stack.
* **`CALL_BUILTIN_PROC`**:
    * **Operands:** 2-byte builtin ID, 2-byte name constant index, 1-byte argument count.
    * **Action:** Calls a built-in procedure that returns `void`. Similar to `CALL_BUILTIN` but uses a numeric builtin ID for faster dispatch and does not push a return value. The name constant index is retained for error messages.
* **`CALL_HOST`**:
    * **Operands:** 1-byte host function ID (from the `HostFunctionID` enum).
    * **Action:** Calls a C function registered in the VM's `host_functions` array. The host function receives the `VM*` pointer and returns a `Value`. The result is pushed onto the stack. Host function IDs above `HOST_FN_COUNT` or unregistered IDs raise a runtime error. The VM releases the globals mutex before calling host functions that may block (e.g., thread waits).
* **`RETURN`**:
    * **Operands:** None.
    * **Action:** Returns from the current function or procedure. If it's a function, the return value is expected to be on top of the stack (unless `discard_result_on_return` is set, in which case it is dropped).
* **`EXIT`**:
    * **Operands:** None.
    * **Action:** Performs an early return from the current function or procedure without halting the entire VM.

---
**Example: Function Call**

Finally, let's look at a function call:

```pascal
MyFunction(a, b);
```

1.  `GET_GLOBAL <index_of_a>`
    * **Action:** Push the first argument (`a`) onto the stack.
    * **Stack:** `[8]`
2.  `GET_GLOBAL <index_of_b>`
    * **Action:** Push the second argument (`b`) onto the stack.
    * **Stack:** `[8, 8]`
3.  `CALL <name_index> <address> <arg_count>`
    * **Action:** The VM uses the `CALL` instruction to execute the function.
        * **`<name_index>`:** An index into the constant pool for the function's name (used for disassembly and debugging; read and discarded at runtime).
        * **`<address>`:** The bytecode address of the first instruction of `MyFunction`. The VM jumps to this address.
        * **`<arg_count>`:** The number of arguments (2 in this case). The VM knows to use the top 2 values on the stack as the arguments for the new function's stack frame.

---

**Example: Indirect Call (Function Pointer)**

```pascal
type TCallback = procedure(x: integer);
var
  fn: TCallback;
begin
  fn := @MyProc;
  fn(42);
end.
```

1. Obtain function pointer and store in `fn`.
2. `GET_GLOBAL <index_of_fn>` — push the function pointer.
3. `PUSH_IMMEDIATE_INT8 42` — push the argument.
4. `PROC_CALL_INDIRECT 1` — call through the pointer, discarding any return value.

---

#### **Threading Opcodes**

* **`THREAD_CREATE`**:
    * **Operands:** 2-byte bytecode address.
    * **Action:** Starts a new thread at the given instruction and pushes its thread identifier. The new thread runs in its own VM instance. If the worker pool has idle threads, one is reused.
* **`THREAD_JOIN`**:
    * **Operands:** None.
    * **Action:** Pops a thread identifier and waits for that thread to finish, yielding control if it is still running.

#### **Synchronization Opcodes**

* **`MUTEX_CREATE`**:
    * **Operands:** None.
    * **Action:** Creates a standard mutex and pushes its integer identifier on the stack.
* **`RCMUTEX_CREATE`**:
    * **Operands:** None.
    * **Action:** Creates a recursive mutex and pushes its integer identifier.
* **`MUTEX_LOCK`**:
    * **Operands:** None (uses mutex id on stack).
    * **Action:** Pops a mutex identifier and blocks until that mutex is acquired.
* **`MUTEX_UNLOCK`**:
    * **Operands:** None (uses mutex id on stack).
    * **Action:** Pops a mutex identifier and releases the corresponding mutex.
* **`MUTEX_DESTROY`**:
    * **Operands:** None (uses mutex id on stack).
    * **Action:** Pops a mutex identifier and destroys the corresponding mutex.

#### **I/O and Miscellaneous Opcodes**

* **`HALT`**:
    * **Operands:** None.
    * **Action:** Stops the VM's execution.
* **`FORMAT_VALUE`**:
    * **Operands:** 1-byte width, 1-byte precision.
    * **Action:** Pops a value and formats it into a string with the specified width and precision. Pushes the formatted string back onto the stack.

---

### **Opcode Summary Table**

| Category | Opcodes |
|---|---|
| **Immediate Constants** | `CONSTANT`, `CONSTANT16`, `CONST_0`, `CONST_1`, `CONST_TRUE`, `CONST_FALSE`, `PUSH_IMMEDIATE_INT8` |
| **Stack Manipulation** | `POP`, `SWAP`, `DUP` |
| **Arithmetic** | `ADD`, `SUBTRACT`, `MULTIPLY`, `DIVIDE`, `INT_DIV`, `MOD`, `NEGATE` |
| **Logical / Bitwise** | `NOT`, `TO_BOOL`, `AND`, `OR`, `XOR`, `SHL`, `SHR` |
| **Comparison** | `EQUAL`, `NOT_EQUAL`, `GREATER`, `GREATER_EQUAL`, `LESS`, `LESS_EQUAL` |
| **Control Flow** | `JUMP`, `JUMP_IF_FALSE` |
| **Global Variables** | `DEFINE_GLOBAL`, `DEFINE_GLOBAL16`, `GET_GLOBAL`, `SET_GLOBAL`, `GET_GLOBAL_ADDRESS`, `GET_GLOBAL16`, `SET_GLOBAL16`, `GET_GLOBAL_ADDRESS16`, `GET_GLOBAL_CACHED`, `SET_GLOBAL_CACHED`, `GET_GLOBAL16_CACHED`, `SET_GLOBAL16_CACHED` |
| **Local Variables** | `GET_LOCAL`, `SET_LOCAL`, `GET_LOCAL_ADDRESS`, `INC_LOCAL`, `DEC_LOCAL`, `RESET_LOCAL` |
| **Upvalues (Closures)** | `GET_UPVALUE`, `SET_UPVALUE`, `GET_UPVALUE_ADDRESS` |
| **Initialization** | `INIT_LOCAL_ARRAY`, `INIT_LOCAL_FILE`, `INIT_LOCAL_POINTER`, `INIT_LOCAL_STRING`, `INIT_FIELD_ARRAY` |
| **Objects** | `ALLOC_OBJECT`, `ALLOC_OBJECT16`, `GET_FIELD_ADDRESS`, `GET_FIELD_ADDRESS16`, `GET_FIELD_ADDRESS_KEEP`, `GET_FIELD_ADDRESS_KEEP16`, `GET_FIELD_OFFSET`, `GET_FIELD_OFFSET16`, `LOAD_FIELD_VALUE`, `LOAD_FIELD_VALUE16`, `LOAD_FIELD_VALUE_BY_NAME`, `LOAD_FIELD_VALUE_BY_NAME16` |
| **Arrays / Strings** | `GET_ELEMENT_ADDRESS`, `GET_ELEMENT_ADDRESS_CONST`, `LOAD_ELEMENT_VALUE`, `LOAD_ELEMENT_VALUE_CONST`, `GET_CHAR_ADDRESS`, `GET_CHAR_FROM_STRING` |
| **Pointer Access** | `SET_INDIRECT`, `GET_INDIRECT` |
| **Set Membership** | `IN` |
| **Function Calls** | `CALL`, `CALL_USER_PROC`, `CALL_INDIRECT`, `PROC_CALL_INDIRECT`, `CALL_METHOD`, `CALL_BUILTIN`, `CALL_BUILTIN_PROC`, `CALL_HOST`, `RETURN`, `EXIT` |
| **Threading** | `THREAD_CREATE`, `THREAD_JOIN` |
| **Synchronization** | `MUTEX_CREATE`, `RCMUTEX_CREATE`, `MUTEX_LOCK`, `MUTEX_UNLOCK`, `MUTEX_DESTROY` |
| **Miscellaneous** | `HALT`, `FORMAT_VALUE`, `CALL_HOST` |

For a catalog of VM built-ins available to front ends, see
[`pscal_vm_builtins.md`](pscal_vm_builtins.md). For guidance on creating
new front ends, consult
[`standalone_vm_frontends.md`](standalone_vm_frontends.md).
