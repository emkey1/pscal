# Rea Front End

This directory hosts the experimental front end for the Rea programming
language. At the moment the executable only loads a source file and executes
an empty bytecode chunk, but the layout below sketches the path toward a full
compiler.

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

