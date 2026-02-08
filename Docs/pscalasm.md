# pscalasm — Reassemble Bytecode From `pscald --asm`

## Name

`pscalasm` — rebuild a `.pbc` bytecode file from a `PSCALASM` block emitted by `pscald`

## Synopsis

```sh
pscalasm <disassembly.txt|-> <output.pbc>
pscalasm --help
```

## Description

`pscalasm` reconstructs PSCAL bytecode from the `PSCALASM` block appended by:

```sh
pscald --asm <input.pbc>
```

The tool is designed for bytecode round-tripping and transport workflows.

Important: `pscalasm` does not currently assemble from opcode mnemonics alone.
It reads the explicit hex-byte block between:

- `== PSCALASM BEGIN v1 ==`
- `== PSCALASM END ==`

## Basic Workflow

1. Dump disassembly plus assembler block:

```sh
pscald --asm input.pbc 2> dump.txt
```

2. Rebuild bytecode:

```sh
pscalasm dump.txt rebuilt.pbc
```

3. Optional verification:

```sh
cmp -s input.pbc rebuilt.pbc && echo OK
```

## Stdin Mode

Use `-` to read from stdin:

```sh
pscald --asm input.pbc 2> dump.txt
cat dump.txt | pscalasm - rebuilt.pbc
```

## Exit Status

- `0` on success.
- Non-zero on parse errors, missing `PSCALASM` block, byte-count mismatch, or file write errors.

## Troubleshooting

- `pscalasm: no PSCALASM block found`
  - Input does not contain a `pscald --asm` block.
  - Re-run `pscald` with `--asm` and capture stderr.

- `byte count mismatch`
  - The block was truncated or edited.
  - Regenerate the dump and retry.

## See Also

- `pscald` — bytecode disassembler (`--asm` producer)
- `pscalvm` — bytecode execution runtime
