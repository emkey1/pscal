# Pscal Virtual Machine Documentation

### **Pscal VM Architecture**

The Pscal VM is a **stack-based virtual machine**. This means that it uses a stack data structure to store and manipulate data during program execution. Instead of operating on registers like a physical CPU, most instructions (opcodes) operate on values at the top of the stack.

#### **Core Components**

The VM's architecture is defined by the `VM` struct in `src/vm/vm.h` and consists of the following key components:

* **Instruction Pointer (IP):** A pointer (`ip`) that always points to the *next* bytecode instruction to be executed.
* **Operand Stack:** A fixed-size array (`stack`) that holds `Value` structs. `Value` is a versatile struct that can represent all of Pscal's data types, including integers, reals, strings, pointers, and more.
    * **`stackTop`:** A pointer to the next available slot on the stack. When a value is pushed, it's placed where `stackTop` points, and then `stackTop` is incremented.
* **Call Stack (Frames):** An array of `CallFrame` structs (`frames`) that manages function and procedure calls. Each time a function is called, a new `CallFrame` is pushed onto this stack. A `CallFrame` contains:
    * **`return_address`**: The IP in the caller to return to when the function finishes.
    * **`slots`**: A pointer to the beginning of the current function's section on the main operand stack. This marks the start of its parameters and local variables.
    * **`function_symbol`**: A pointer to the `Symbol` table entry for the function being called, which provides metadata like the number of local variables and upvalues.
* **Symbol Tables:** The VM maintains pointers to two crucial hash tables:
    * **`vmGlobalSymbols`:** A `HashTable` for runtime storage and lookup of global variables.
    * **`procedureTable`:** A `HashTable` that stores information about all compiled procedures and functions, which is used for disassembly and resolving calls.
* **Bytecode Chunk:** A pointer (`chunk`) to the `BytecodeChunk` being executed. This chunk contains the bytecode instructions (`code`), the constant pool (`constants`), and line number information for debugging (`lines`).
* **Thread Table:** The VM spawns real OS-level threads via `pthread`. Each entry in the `threads` array holds a native thread and its own `VM` instance, allowing bytecode routines to execute in parallel.
* **Mutex Table:** Runtime-created mutex objects live in the `mutexes` array. Each slot tracks a native `pthread_mutex_t` along with whether it is active, enabling synchronization between threads.

#### **Execution Flow**

The VM's execution is driven by the `interpretBytecode` function in `src/vm/vm.c`. This function contains a main loop that repeatedly performs the following steps:

1.  Reads the next bytecode instruction pointed to by `ip`.
2.  Decodes the instruction and its operands.
3.  Performs the operation defined by the instruction, which typically involves pushing, popping, or manipulating values on the stack.
4.  Updates the instruction pointer to the next instruction.
5.  This loop continues until it encounters an `OP_HALT` instruction or a `OP_RETURN` from the main program body.

---

### **Opcode Reference**

The following is a complete list of opcodes supported by the Pscal VM, as defined in `src/compiler/bytecode.h`, with integrated examples of their usage.

#### **Stack Manipulation Opcodes**

* **`OP_CONSTANT`**:
    * **Operands:** 1-byte index into the constant pool.
    * **Action:** Pushes the constant value at the specified index onto the stack.
* **`OP_CONSTANT16`**:
    * **Operands:** 2-byte index into the constant pool.
    * **Action:** Same as `OP_CONSTANT`, but for constant pools with more than 256 entries.
* **`OP_POP`**:
    * **Operands:** None.
    * **Action:** Pops the top value from the stack and discards it.
* **`OP_SWAP`**:
    * **Operands:** None.
    * **Action:** Swaps the top two values on the stack.
* **`OP_DUP`**:
    * **Operands:** None.
    * **Action:** Duplicates the top value on the stack.

#### **Arithmetic and Logical Opcodes**

* **`OP_ADD`**, **`OP_SUBTRACT`**, **`OP_MULTIPLY`**, **`OP_DIVIDE`**, **`OP_INT_DIV`**, **`OP_MOD`**:
    * **Operands:** None.
    * **Action:** Pop two values from the stack, perform the specified arithmetic operation, and push the result. `OP_DIVIDE` always produces a real result, while `OP_INT_DIV` performs integer division. `OP_ADD` is also overloaded for string concatenation.
* **`OP_NEGATE`**:
    * **Operands:** None.
    * **Action:** Pops one value, negates it, and pushes the result.
* **`OP_NOT`**:
    * **Operands:** None.
    * **Action:** Pops one boolean value, inverts it, and pushes the result.
* **`OP_AND`**, **`OP_OR`**, **`OP_SHL`**, **`OP_SHR`**:
    * **Operands:** None.
    * **Action:** Pop two integer values, perform the specified bitwise operation, and push the result. `OP_AND` and `OP_OR` also support logical operations on booleans.
* **`OP_EQUAL`**, **`OP_NOT_EQUAL`**, **`OP_GREATER`**, **`OP_GREATER_EQUAL`**, **`OP_LESS`**, **`OP_LESS_EQUAL`**:
    * **Operands:** None.
    * **Action:** Pop two values, perform the specified comparison, and push the boolean result (`true` or `false`).

---
**Example: Arithmetic and Assignment**

Consider the following line of Pascal code:

```pascal
a := 5 + 3;
```

The compiler would translate this into the following sequence of bytecode instructions:

1.  `OP_CONSTANT <index_of_5>`
    * **Action:** The VM pushes the integer value `5` from the constant pool onto the stack.
    * **Stack:** `[5]`
2.  `OP_CONSTANT <index_of_3>`
    * **Action:** The VM pushes the integer value `3` from the constant pool onto the stack.
    * **Stack:** `[5, 3]`
3.  `OP_ADD`
    * **Action:** The VM pops the top two values (`3` and `5`), adds them together, and pushes the result (`8`) back onto the stack.
    * **Stack:** `[8]`
4.  `OP_SET_GLOBAL <index_of_a>`
    * **Action:** The VM pops the result (`8`) from the stack and stores it in the global variable `a`.
    * **Stack:** `[]`
---

#### **Control Flow Opcodes**

* **`OP_JUMP`**:
    * **Operands:** 2-byte signed offset.
    * **Action:** Unconditionally jumps the instruction pointer by the specified offset.
* **`OP_JUMP_IF_FALSE`**:
    * **Operands:** 2-byte signed offset.
    * **Action:** Pops a value from the stack. If the value is `false` (or numerically zero), jumps the instruction pointer by the specified offset.

---
**Example: Conditional Logic**

Here's how an `if` statement would be compiled:

```pascal
if a > b then
  c := 10;
```

1.  `OP_GET_GLOBAL <index_of_a>`
    * **Action:** Push the value of `a`.
    * **Stack:** `[8]`
2.  `OP_GET_GLOBAL <index_of_b>`
    * **Action:** Push the value of `b`.
    * **Stack:** `[8, 8]`
3.  `OP_GREATER`
    * **Action:** Pop `8` and `8`, compare them (`8 > 8` is false), and push the boolean result.
    * **Stack:** `[false]`
4.  `OP_JUMP_IF_FALSE <offset>`
    * **Action:** Pop the boolean value. Since it's `false`, the VM jumps the instruction pointer forward by the specified offset, skipping the code for the `then` block.
    * **Stack:** `[]`

If `a` had been greater than `b`, the `OP_JUMP_IF_FALSE` would not have jumped, and the code to assign `10` to `c` would have been executed.

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

1. `OP_CONSTANT <index_of_0>`
   * Push integer `0`.
2. `OP_SET_GLOBAL <index_of_i>`
   * Store in variable `i`.
3. loop_start:
   * `OP_GET_GLOBAL <index_of_i>`
   * `OP_CONSTANT <index_of_3>`
   * `OP_LESS`
   * `OP_JUMP_IF_FALSE <exit_offset>`
4. loop_body:
   * `OP_GET_GLOBAL <index_of_i>`
   * `OP_WRITE_LN 1`
   * `OP_GET_GLOBAL <index_of_i>`
   * `OP_CONSTANT <index_of_1>`
   * `OP_ADD`
   * `OP_SET_GLOBAL <index_of_i>`
   * `OP_JUMP <loop_start>`
5. exit:
   * (next instruction after loop)

This sequence uses `OP_JUMP_IF_FALSE` to exit the loop and `OP_JUMP` to repeat.

---

#### **Variable and Data Structure Opcodes**

* **`OP_DEFINE_GLOBAL`** / **`OP_DEFINE_GLOBAL16`**:
    * **Operands:** Variable-length. Includes a constant index for the variable's name (8-bit for `OP_DEFINE_GLOBAL`, 16-bit for `OP_DEFINE_GLOBAL16`), the variable's type, and additional type information (e.g., array dimensions, record structure).
    * **Action:** Defines a new global variable in the VM's global symbol table.
* **`OP_GET_GLOBAL`** / **`OP_GET_GLOBAL16`**:
    * **Operands:** 8-bit or 16-bit constant index for the variable's name.
    * **Action:** Pushes the value of the specified global variable onto the stack.
* **`OP_SET_GLOBAL`** / **`OP_SET_GLOBAL16`**:
    * **Operands:** 8-bit or 16-bit constant index for the variable's name.
    * **Action:** Pops a value from the stack and assigns it to the specified global variable.
* **`OP_GET_GLOBAL_ADDRESS`** / **`OP_GET_GLOBAL_ADDRESS16`**:
    * **Operands:** 8-bit or 16-bit constant index for the variable's name.
    * **Action:** Pushes a pointer to the specified global variable's `Value` struct onto the stack.
* **`OP_GET_LOCAL`** / **`OP_SET_LOCAL`**:
    * **Operands:** 1-byte slot index within the current call frame.
    * **Action:** `OP_GET_LOCAL` pushes the value of the local variable at the given slot. `OP_SET_LOCAL` pops a value and assigns it to the local variable.
* **`OP_GET_LOCAL_ADDRESS`**:
    * **Operands:** 1-byte slot index.
    * **Action:** Pushes a pointer to the specified local variable's `Value` struct onto the stack.
* **`OP_GET_UPVALUE`** / **`OP_SET_UPVALUE`**:
    * **Operands:** 1-byte upvalue index.
    * **Action:** Accesses a variable from an enclosing function's scope (a "closure"). `OP_GET_UPVALUE` pushes the value, and `OP_SET_UPVALUE` assigns to it.
* **`OP_GET_UPVALUE_ADDRESS`**:
    * **Operands:** 1-byte upvalue index.
    * **Action:** Pushes a pointer to the specified upvalue's `Value` struct.
* **`OP_INIT_LOCAL_ARRAY`**, **`OP_INIT_LOCAL_FILE`**, **`OP_INIT_LOCAL_POINTER`**:
    * **Operands:** Variable-length, including a slot index and type metadata.
    * **Action:** Initializes a local variable of a complex type (array, file, or pointer) at the specified slot. For arrays, any dimension using the sentinel bound index `0xFFFF` will pop its size from the stack (treated as an upper bound plus one) and assume a lower bound of `0`.
* **`OP_GET_FIELD_ADDRESS`** / **`OP_GET_FIELD_ADDRESS16`**:
    * **Operands:** Constant index for the field's name.
    * **Action:** Pops a record or a pointer to a record from the stack and pushes a pointer to the specified field's `Value` struct.
* **`OP_GET_ELEMENT_ADDRESS`**:
    * **Operands:** 1-byte dimension count.
    * **Action:** Pops an array or pointer to an array, and then pops the indices for each dimension. Pushes a pointer to the specified element's `Value` struct.
* **`OP_GET_CHAR_ADDRESS`**:
    * **Operands:** None.
    * **Action:** Pops an index and a pointer to a string. Pushes a pointer to the character at that index within the string.
* **`OP_SET_INDIRECT`**:
    * **Operands:** None.
    * **Action:** Pops a value and a pointer. Assigns the value to the memory location indicated by the pointer.
* **`OP_GET_INDIRECT`**:
    * **Operands:** None.
    * **Action:** Pops a pointer and pushes a copy of the value it points to.
* **`OP_IN`**:
    * **Operands:** None.
    * **Action:** Pops an item and a set. Pushes `true` if the item is in the set, `false` otherwise.
* **`OP_GET_CHAR_FROM_STRING`**:
    * **Operands:** None.
    * **Action:** Pops an index and a string. Pushes the character at that index.

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

1. `OP_GET_GLOBAL_ADDRESS <index_of_p>`
   * Push a pointer to the global variable `p`.
2. `OP_GET_FIELD_ADDRESS <index_of_x>`
   * Pop the record pointer, push a pointer to field `x`.
3. `OP_CONSTANT <index_of_10>`
   * Push the integer constant `10`.
4. `OP_SET_INDIRECT`
   * Pop value and pointer; store `10` in `p.x`.

---

#### **Function and Procedure Call Opcodes**

* **`OP_CALL`**:
    * **Operands:** 2-byte name index (for disassembly), 2-byte bytecode address, 1-byte argument count.
    * **Action:** Calls a user-defined function or procedure at the specified address.
* **`OP_CALL_BUILTIN`**:
    * **Operands:** 2-byte name index, 1-byte argument count.
    * **Action:** Calls a built-in function or procedure by name.
* **`OP_RETURN`**:
    * **Operands:** None.
    * **Action:** Returns from the current function or procedure. If it's a function, it expects the return value to be on top of the stack.
* **`OP_EXIT`**:
    * **Operands:** None.
    * **Action:** Performs an early return from the current function or procedure.

---
**Example: Function Call**

Finally, let's look at a function call:

```pascal
MyFunction(a, b);
```

1.  `OP_GET_GLOBAL <index_of_a>`
    * **Action:** Push the first argument (`a`) onto the stack.
    * **Stack:** `[8]`
2.  `OP_GET_GLOBAL <index_of_b>`
    * **Action:** Push the second argument (`b`) onto the stack.
    * **Stack:** `[8, 8]`
3.  `OP_CALL <name_index> <address> <arg_count>`
    * **Action:** The VM uses the `OP_CALL` instruction to execute the function.
        * **`<name_index>`:** An index into the constant pool for the function's name (used for disassembly and debugging).
        * **`<address>`:** The bytecode address of the first instruction of `MyFunction`. The VM jumps to this address.
        * **`<arg_count>`:** The number of arguments (2 in this case). The VM knows to use the top 2 values on the stack as the arguments for the new function's stack frame.
---

#### **Threading Opcodes**

* **`OP_THREAD_CREATE`**:
    * **Operands:** 2-byte bytecode address.
    * **Action:** Starts a new thread at the given instruction and pushes its thread identifier.
* **`OP_THREAD_JOIN`**:
    * **Operands:** None.
    * **Action:** Pops a thread identifier and waits for that thread to finish, yielding control if it is still running.

#### **Synchronization Opcodes**

* **`OP_MUTEX_CREATE`**:
    * **Operands:** None.
    * **Action:** Creates a standard mutex and pushes its integer identifier on the stack.
* **`OP_RCMUTEX_CREATE`**:
    * **Operands:** None.
    * **Action:** Creates a recursive mutex and pushes its integer identifier.
* **`OP_MUTEX_LOCK`**:
    * **Operands:** None (uses mutex id on stack).
    * **Action:** Pops a mutex identifier and blocks until that mutex is acquired.
* **`OP_MUTEX_UNLOCK`**:
    * **Operands:** None (uses mutex id on stack).
    * **Action:** Pops a mutex identifier and releases the corresponding mutex.

#### **I/O and Miscellaneous Opcodes**

* **`OP_WRITE_LN`** / **`OP_WRITE`**:
    * **Operands:** 1-byte argument count.
    * **Action:** Pops the specified number of arguments from the stack and prints them to the console. `OP_WRITE_LN` adds a newline.
* **`OP_CALL_HOST`**:
    * **Operands:** 1-byte host function ID.
    * **Action:** Calls a C function that is registered with the VM.
* **`OP_HALT`**:
    * **Operands:** None.
    * **Action:** Stops the VM's execution.
* **`OP_FORMAT_VALUE`**:
    * **Operands:** 1-byte width, 1-byte precision.
    * **Action:** Pops a value and formats it into a string with the specified width and precision. Pushes the formatted string back onto the stack.

For a catalog of VM built-ins available to front ends, see
[`pscal_vm_builtins.md`](pscal_vm_builtins.md). For guidance on creating
new front ends, consult
[`standalone_vm_frontends.md`](standalone_vm_frontends.md).
