# Aether Front-End Bootstrap

Aether is being introduced as a new front end for the existing PSCAL toolchain.

Current constraints:

- Aether targets the existing PSCAL VM and shared bytecode compiler.
- Shared compiler and VM changes should stay minimal, concise, and clean.
- Source syntax decisions should optimize for correctness per token, not just
  shortest-possible source, while remaining readable to a skilled human.

Bootstrap status:

- `build/bin/aether` now exists as a distinct front-end target.
- The target currently reuses the Rea front-end implementation as a bootstrap
  so build, packaging, and CLI integration can be exercised without forking the
  bytecode compiler.
- `@pre` and `@post` now lower through the Aether rewrite path into ordinary
  guard code, so contract failures are enforced without backend changes.
- A first `fx` validation pass now rejects selected effectful builtin calls
  outside `fx { ... }` blocks before the shared Rea semantic pass runs.
- `@pure` now has a first semantic pass as well: pure functions reject direct
  calls to effectful builtins and direct calls into known non-pure Aether
  functions.
- A restricted `par { ... }` form now lowers to shared `spawn`/`join`
  semantics for direct call statements, without adding runtime machinery.
- The next phase is to replace the shared Rea grammar incrementally with
  Aether-specific lexer, parser, and semantic logic.

Planned near-term work:

1. Define and stabilize a small Aether Core before expanding surface area.
2. Introduce the Aether lexer/token set with compact, regular syntax.
3. Parse Aether declarations and control-flow into the shared PSCAL AST.
4. Enforce explicit effect regions in semantic analysis.
5. Lower contracts (`@pre`, `@post`, `@cost`, `@pure`) cleanly onto the
   existing compiler/runtime path.
6. Add Aether-specific tests and library fixtures under `Tests/`.

Roadmap item:

- Optional cryptographic signing for compiled bytecode should be added as a
  future format extension. The goal is integrity verification, not
  obfuscation. Signing should layer onto the existing bytecode serialization
  path so unsigned artifacts continue to work.
