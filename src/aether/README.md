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
- Aether-native `print(...)` and `println(...)` spellings now lower onto the
  shared `write` and `writeln` builtins and participate in the same `fx` and
  `@pure` validation rules.
- `@pure` now has a first semantic pass as well: pure functions reject direct
  calls to effectful builtins and direct calls into known non-pure Aether
  functions.
- A restricted `par { ... }` form now lowers to shared `spawn`/`join`
  semantics for direct call statements, without adding runtime machinery.
- A compact half-open range loop form `for name in start..end { ... }` now
  lowers onto the shared Rea `for (...)` form.
- Imported modules now parse through the active frontend parser as well, so
  Aether module files can use `mod`, `use`, and `export fn` syntax instead of
  needing raw Rea syntax in imported sources.
- Embedded `toon:` blocks now preserve real TOON syntax based on the upstream
  TOON specification and currently lower to escaped `str` constants for `TOON`
  values, keeping the backend unchanged while the frontend syntax settles.
- Compact `toon_*` helper spellings now lower onto the shared `Json` library
  only for the parts that map cleanly today: parse/root/close on
  JSON-compatible literal payload text.
- Keyed scalar helpers such as `toon_get_text(root, "name")`,
  `toon_get_int(root, "count")`, and `toon_get_bool(root, "enabled")` now
  lower directly onto yyjson key lookup plus scalar extraction.
- `has_toon()` now lowers to the shared yyjson availability probe instead of
  exposing raw `hasextbuiltin("yyjson", "YyjsonRead")` in Aether source.
- `ToonDoc` and `ToonNode` are now valid source-level opaque handle types for
  parsed TOON documents and nodes, while still lowering to the shared runtime
  representation during the bootstrap phase.
- A first semantic fence now rejects obvious arithmetic misuse of
  `ToonDoc` / `ToonNode` bindings in Aether source.
- Aether also now rejects direct cross-assignment between `ToonDoc` and
  `ToonNode` bindings at the source level.
- TOON helper calls now also validate obvious handle-kind mismatches such as
  passing a `ToonDoc` where a `ToonNode` is expected, or vice versa.
- Bindings initialized from `toon_parse`, `toon_root`, `toon_key`, and
  `toon_at` now also require the matching `ToonDoc` / `ToonNode` source type.
- Scalar bindings initialized or reassigned from `toon_get_*` and
  `toon_*_value` helpers, along with `toon_len(...)`, must also use matching
  `Text`, `Int`, `Real`, or `Bool` source types.
- That typed scalar model now has direct example/test coverage for text, int,
  real, bool, length, and null-shape helpers.
- Aether now also exposes node inspection helpers like `toon_type(...)` and
  `toon_is_text` / `toon_is_int` / `toon_is_real` / `toon_is_bool` /
  `toon_is_null` / `toon_is_arr` / `toon_is_obj`.
- It also now exposes compact presence helpers: `toon_has_key(...)` for object
  membership and `toon_has_at(...)` for array index existence checks.
- For common fallback-heavy automation flows, Aether now also exposes
  `toon_get_text_or(...)`, `toon_get_int_or(...)`, `toon_get_real_or(...)`,
  and `toon_get_bool_or(...)` for typed scalar lookup with defaults.
- Once those scalar bindings are known, direct assignment between them must
  also preserve the same source-level scalar type.
- Keyed and indexed TOON helpers now also validate typed variable arguments:
  `toon_key(...)` and `toon_get_*` expect `Text` keys, while `toon_at(...)`
  expects an `Int` index when those arguments come from bindings.
- `toon_parse(...)` now also validates named payload bindings: the first
  argument must be `Text` or `TOON` when it comes from a source-level binding.
- `toon_parse_file(...)` likewise requires a `Text` file-path binding when its
  first argument comes from a named source-level binding.
- Direct handle-oriented TOON helpers now also lower cleanly onto existing
  yyjson builtins: key lookup, array indexing/length, scalar extraction, and
  handle cleanup.
- Simple variable-based `toon_parse(...)` flows now work too when the payload
  can be traced to a local `Text` or `TOON` literal binding by the rewrite
  layer.
- Compact `type Name { ... }` blocks now lower to the shared Rea class model,
  and `field: Type;` entries inside those blocks become ordinary class fields.
- A compact initializer form `let value: Type = Type { field: expr, ... };`
  now lowers to shared object construction plus explicit field assignments.
- `self` is now the Aether source spelling for the current object inside
  methods and lowers to the shared backend's `myself` identifier.
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
