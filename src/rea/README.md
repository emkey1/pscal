# Rea Front End

This directory hosts the experimental front end for the Rea programming
language. The parser now emits PSCAL AST nodes directly, mirroring the Pascal
front end, so the resulting tree can be compiled without an intermediate
conversion step. The current implementation handles basic expressions, and the
layout below sketches the path toward a full compiler.

Numeric literals default to 64-bit integers or doubles, preserving the full
range and precision of source values.

## Running

The `rea` front end supports a few diagnostic options:

```
--dump-ast-json        Dump the parsed AST as JSON and exit.
--dump-bytecode        Dump the compiled bytecode before execution.
--dump-bytecode-only   Dump the compiled bytecode and exit without executing.
```

## Roadmap

- Build a lexer that produces tokens for the syntax described in
  `LANGUAGE_SPEC.md`.
- Implement a recursive-descent parser that constructs an AST.
- Perform semantic analysis with a class-aware symbol table and type system.
- Generate PSCAL VM bytecode, including support for object allocation, field
  access and virtual method dispatch.
- Wire the resulting compiler into the existing build and testing
  infrastructure.

See `LANGUAGE_SPEC.md` for the complete language specification.

