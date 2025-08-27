# Building Custom Frontends for the Stand-alone VM

The stand-alone `pscalvm` executes programs encoded as `.pbc` bytecode. By
producing this format, any compiler or language front end can run on the VM and
reuse its runtime without touching the C sources. For an overview of the VM
itself, see [`pscal_vm_overview.md`](pscal_vm_overview.md).

## End-to-end workflow

1. **Load opcode definitions.** Opcode numbers live in `src/compiler/bytecode.h`.
   A frontend must use the same values as the VM.
2. **Emit instructions and constants.** Build a bytecode stream and a table of
   typed constants referenced by index.
3. **Write the `.pbc` file.** Serialize the bytecode, constants and metadata to
   disk.
4. **Run with `pscalvm`.** Execute the resulting file just like bytecode
   produced by the main Pascal compiler.

## Step-by-step: the tiny compiler (Python)

`tools/tiny` is a compact Python compiler for an educational language. It
illustrates the entire pipeline for targeting the VM.

### Loading opcodes

```python
def load_opcodes(root: Path) -> Dict[str, int]:
    header = root / "src" / "compiler" / "bytecode.h"
    pattern = re.compile(r"^\s*(OP_[A-Z0-9_]+)\s*,")
    opcodes: Dict[str, int] = {}
    enum_started = False
    index = 0
    with open(header, "r", encoding="utf-8") as f:
        for line in f:
            if not enum_started:
                if line.strip().startswith("typedef enum"):
                    enum_started = True
                continue
            if line.strip().startswith("}"):
                break
            m = pattern.search(line)
            if m:
                name = m.group(1)
                opcodes[name] = index
                index += 1
    return opcodes
```

The script parses `bytecode.h` at build time so it always uses the same numeric
opcode values as the VM.

### Preparing constants and builtins

```python
self.read_idx = self.builder.add_constant(TYPE_STRING, "read")
self.type_integer_idx = self.builder.add_constant(TYPE_STRING, "integer")
```

`BytecodeBuilder` stores both the instruction bytes and a table of typed
constants. Builtin routines such as `read` are inserted into this table as
strings.

### Compiling statements

When a `read` statement is parsed the compiler emits an instruction to call the
builtin:

```python
self.builder.emit(self.opcodes["OP_CALL_BUILTIN"])
self.builder.emit_short(self.read_idx)
self.builder.emit(1)  # argument count
```

Other statements translate into sequences of opcodes in the same fashion.

### Writing `.pbc` bytecode

```python
with open(path, "wb") as f:
    f.write(struct.pack("<II", CACHE_MAGIC, CACHE_VERSION))
    f.write(struct.pack("<ii", len(code), len(consts)))
    f.write(code)
    f.write(struct.pack("<" + "i" * len(lines), *lines))
    for ctype, val in consts:
        f.write(struct.pack("<i", ctype))
        if ctype == TYPE_INTEGER:
            f.write(struct.pack("<q", int(val)))
        elif ctype == TYPE_STRING:
            if val is None:
                f.write(struct.pack("<i", -1))
            else:
                data = val.encode("utf-8")
                f.write(struct.pack("<i", len(data)))
                f.write(data)
```

The builder encodes the bytecode and constant table and writes the final file.
`CACHE_VERSION` also identifies the VM bytecode format. A VM built with an
older version will load the bytecode but emit a warning if it targets a newer
VM. Programs can call `VMVersion` and `BytecodeVersion` to decide whether to
continue or exit. Set `PSCAL_STRICT_VM=1` to force the VM to abort instead.
Run the result with:

```sh
python tools/tiny source.tiny out.pbc
./build/bin/pscalvm out.pbc
```

For example, the following program reads two numbers and prints their sum:

```tiny
read a;
read b;
write a + b;
```

```sh
python tools/tiny add.tiny add.pbc
printf "2\n3\n" | ./build/bin/pscalvm add.pbc
# output: 5
```

Use `pscalvm -d add.pbc` to disassemble the bytecode for debugging.

## Step-by-step: the clike compiler (C)

`src/clike` is a compact C like language compiler. It illustrates the same pipeline 
using C code.

### Loading opcodes

Because the front end is written in C it can include the VM's opcode
definitions directly:

```c
#include "compiler/bytecode.h"
```

This header defines the `OP_*` enum so the compiler and VM share identical
numeric opcode values.

### Preparing constants and builtins

```c
int read_idx = addStringConstant(&chunk, "readln");
int type_integer_idx = addStringConstant(&chunk, "integer");
```

`BytecodeChunk` stores both the instruction bytes and a table of typed
constants. Builtin routines such as `readln` are inserted into this table as
strings.

### Compiling statements

When a `read` statement is parsed the compiler emits an instruction to call the
builtin:

```c
writeBytecodeChunk(&chunk, OP_CALL_BUILTIN, line);
emitShort(&chunk, (uint16_t)read_idx, line);
writeBytecodeChunk(&chunk, 1, line); /* argument count */
```

Other statements translate into sequences of opcodes in the same fashion, as
demonstrated in `src/clike/codegen.c`.

## Calling VM builtins

Opcodes `OP_CALL_BUILTIN` and `OP_CALL_BUILTIN_PROC` invoke the VM's built-in
functions and procedures. The VM exposes a large catalog of routines described in
`Docs/pscal_vm_builtins.md`. To add your own, see
[`extending_builtins.md`](extending_builtins.md).

To invoke a builtin from generated code:

1. Add the builtin name as a string constant.
2. Emit `OP_CALL_BUILTIN` for functions or `OP_CALL_BUILTIN_PROC` for procedures,
   passing the constant index and argument count.
3. At runtime the VM resolves the name and dispatches to the builtin
   implementation.

Example: call the `random` function which returns an integer.

Python:

```python
rand_idx = builder.add_constant(TYPE_STRING, "random")
builder.emit(opcodes["OP_CALL_BUILTIN"])
builder.emit_short(rand_idx)
builder.emit(0)  # no arguments; result left on stack
```

C:

```c
int rand_idx = addStringConstant(&chunk, "random");
writeBytecodeChunk(&chunk, OP_CALL_BUILTIN, line);
emitShort(&chunk, (uint16_t)rand_idx, line);
writeBytecodeChunk(&chunk, 0, line); /* no arguments */
```

Example: call the `halt` procedure that terminates execution.

Python:

```python
halt_idx = builder.add_constant(TYPE_STRING, "halt")
builder.emit(opcodes["OP_CALL_BUILTIN_PROC"])
builder.emit_short(halt_idx)
builder.emit(0)  # no arguments; no return value
```

C:

```c
int halt_idx = addStringConstant(&chunk, "halt");
writeBytecodeChunk(&chunk, OP_CALL_BUILTIN_PROC, line);
emitShort(&chunk, (uint16_t)halt_idx, line);
writeBytecodeChunk(&chunk, 0, line); /* no arguments */
```

Frontends can therefore expose console I/O, math utilities, graphics, networking
and many other features simply by referencing the builtin name and emitting the
proper call opcode. Builtins are resolved dynamically by name, so the VM does
not need recompilation when new frontends are introduced.

For a catalog of available routines, see
[`pscal_vm_builtins.md`](pscal_vm_builtins.md).

