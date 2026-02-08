# pscalasm — Assemble PSCAL Bytecode

## Name

`pscalasm` — assemble a `.pbc` bytecode file from textual PSCAL assembly

## Synopsis

```sh
pscalasm <assembly.txt|-> <output.pbc>
pscalasm --help
```

## Description

`pscalasm` supports two input modes:

1. `PSCALASM2` textual assembly (primary mode).
2. Legacy `PSCALASM` hex block from `pscald --asm` (fallback compatibility mode).

Use `pscald --emit-asm` to export canonical text assembly that round-trips via
`pscalasm`.

## Canonical Workflow

1. Emit canonical assembly:

```sh
pscald --emit-asm input.pbc > dump.asm
```

2. Assemble back to bytecode:

```sh
pscalasm dump.asm rebuilt.pbc
```

3. Optional verification:

```sh
pscald input.pbc 2> a.disasm
pscald rebuilt.pbc 2> b.disasm
diff -u a.disasm b.disasm
```

## Notes On `PSCALASM2` Code Sections

- `code <byte_count>` declares the final byte size of the instruction stream.
- `inst <line> <OPCODE> ...` emits one instruction.
- `label <name>` marks the current instruction boundary.
- `JUMP` and `JUMP_IF_FALSE` accept either:
  - two raw operand bytes (`inst 12 JUMP 255 244`)
  - one signed relative offset (`inst 12 JUMP -12`)
  - one symbolic label (`inst 12 JUMP @loop_start`)

## Notes On `PSCALASM2` Constants

- Primitive constants remain one-line entries (`INT`, `REAL`, `STR`, `CHAR`, `BOOL`, `NIL`).
- Additional supported serialized forms:
  - `ENUM`: `const <idx> 10 "<enum_name>" <ordinal>`
  - `SET`: `const <idx> 14 <count> <ord0> <ord1> ...`
  - `POINTER`:
    - null pointer: `const <idx> 15 null`
    - C-string pointer payload:
      `const <idx> 15 charptr "<string_data>"`
    - shell compiled function pointer:
      `const <idx> 15 shellfn_asm "<escaped_nested_pscalasm2>"`
    - opaque raw pointer address:
      `const <idx> 15 opaque_addr <address>`
  - `ARRAY` (scalar elements):  
    `const <idx> 11 dims <n> elem <var_type> bounds <lb0> <ub0> ... values <total> <v0> <v1> ...`
- Serialized constant globals are preserved with:
  - `const_symbols <count>`
  - `const_symbol "<name>" <var_type> ...payload...`
- Serialized type-table entries are preserved with:
  - `types <count>`
  - `type "<name>" "<json_ast>"`

## Stdin Mode

Use `-` to read from stdin:

```sh
pscald --emit-asm input.pbc | pscalasm - rebuilt.pbc
```

## Exit Status

- `0` on success.
- Non-zero on parse/assembly errors or file I/O failures.

## Legacy Compatibility

Legacy block mode still works:

```sh
pscald --asm input.pbc 2> dump.txt
pscalasm dump.txt rebuilt.pbc
```

## See Also

- `pscald` — disassembler and asm exporter (`--emit-asm`, `--asm`)
- `pscalvm` — bytecode execution runtime
