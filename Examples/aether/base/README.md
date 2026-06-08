# Aether base examples

This directory contains small Aether programs that run with the current
bootstrap frontend:

```sh
./build/bin/aether Examples/aether/base/hello
./build/bin/aether Examples/aether/base/control_flow
./build/bin/aether Examples/aether/base/effects_contracts
./build/bin/aether Examples/aether/base/contracts
./build/bin/aether Examples/aether/base/contract_layouts
./build/bin/aether Examples/aether/base/pure_functions
./build/bin/aether Examples/aether/base/parallel_calls
./build/bin/aether Examples/aether/base/task_helpers
./build/bin/aether Examples/aether/base/ai_helpers
./build/bin/aether Examples/aether/base/for_range
./build/bin/aether Examples/aether/base/module_demo
./build/bin/aether Examples/aether/base/toon_blocks
./build/bin/aether Examples/aether/base/toon_access
./build/bin/aether Examples/aether/base/toon_handles
./build/bin/aether Examples/aether/base/toon_typed_handles
./build/bin/aether Examples/aether/base/toon_rebind
./build/bin/aether Examples/aether/base/toon_scalar_bindings
./build/bin/aether Examples/aether/base/toon_scalar_rebind
./build/bin/aether Examples/aether/base/toon_scalar_alias
./build/bin/aether Examples/aether/base/toon_key_vars
./build/bin/aether Examples/aether/base/toon_parse_payloads
./build/bin/aether Examples/aether/base/toon_parse_file
./build/bin/aether Examples/aether/base/toon_shape_scalars
./build/bin/aether Examples/aether/base/toon_real_scalars
./build/bin/aether Examples/aether/base/toon_type_checks
./build/bin/aether Examples/aether/base/toon_presence_checks
./build/bin/aether Examples/aether/base/toon_defaults
./build/bin/aether Examples/aether/base/cost_annotations
./build/bin/aether Examples/aether/base/toon_variable_parse
./build/bin/aether Examples/aether/base/type_blocks
./build/bin/aether Examples/aether/base/type_init
./build/bin/aether Examples/aether/base/self_alias
```

These examples stay within the currently supported Aether Core subset:

- `const` and `let` declarations with types
- `fn ... -> Type`
- `ret`
- `if` and `while` without mandatory parentheses
- `fx { ... }`
- known effectful builtins such as `write`, `writeln`, `printf`, `readln`,
  `halt`, and thread-launch helpers require `fx`
- Aether-native `print(...)` and `println(...)` spellings lowered onto the
  shared `write` and `writeln` builtins
- `ai_chat(...)` lowered onto the shared OpenAI chat-completions builtin for
  compact agent/tooling-oriented source
- compact task/thread helpers such as `task_spawn`, `task_queue`, `task_wait`,
  `task_lookup`, `task_stats`, and `task_stats_json` lowered onto the shared
  worker-pool/runtime thread helpers
- `@pure` now rejects direct effectful builtin calls and direct calls into
  non-pure Aether functions
- restricted `par { ... }` lowering for direct call statements, mapped onto
  shared spawn/join behavior
- half-open `for name in start..end { ... }` range loops lowered onto shared
  `for (...)` semantics
- `mod`, `use`, and `export fn` lowered onto the shared module/import system
- embedded `toon:` blocks that carry upstream TOON syntax and currently lower
  to `TOON`/`Text` string payloads
- compact `toon_*` helper calls lowered onto the shared `Json` library for
  parse/root/key/array access when yyjson support is available; this helper
  path currently covers parse/root/close cleanly for direct literal payloads
  and expects JSON-compatible `TOON` text
- keyed scalar helpers such as `toon_get_text(root, "name")` and
  `toon_get_int(root, "count")` lowered onto yyjson key lookup plus scalar
  extraction
- `has_toon()` as the compact Aether capability probe for TOON/yyjson support
- `ToonDoc` and `ToonNode` as source-level opaque handle types for parsed TOON
  documents and nodes, lowered onto the shared runtime representation
- `toon_typed_handles`: combines `has_toon()`, `ToonDoc`, `ToonNode`,
  handle navigation, keyed scalar reads, and explicit cleanup in one example
- `toon_rebind`: shows a `ToonNode` binding being reassigned from another
  node-producing helper while preserving the source-level handle kind rules
- `toon_scalar_bindings`: binds `toon_get_text` / `toon_get_int` /
  `toon_get_bool` results into matching `Text` / `Int` / `Bool` variables
- `toon_scalar_rebind`: reassigns a `Bool` binding from `toon_get_bool(...)`
  while preserving the declared scalar type
- `toon_scalar_alias`: reuses `Text` and `Int` scalar bindings through direct
  assignment while preserving Aether's source-level scalar types
- `toon_key_vars`: uses typed `Text` key bindings and an `Int` index binding
  with `toon_key(...)`, `toon_at(...)`, and `toon_get_text(...)`
- `toon_parse_payloads`: parses TOON from named `Text` / `TOON` payload
  bindings and then reads typed values from the parsed document
- `toon_parse_file`: parses TOON/JSON from a named `Text` file path binding
  and then reads typed values from the parsed document
- `toon_shape_scalars`: binds `toon_len(...)` and `toon_null_value(...)` into
  matching `Int` and `Bool` variables
- `toon_real_scalars`: binds `toon_get_real(...)` and `toon_real_value(...)`
  into `Real` variables
- `toon_type_checks`: uses `toon_type(...)` and `toon_is_*` helpers to inspect
  node kinds through Aether-native names
- `toon_presence_checks`: uses `toon_has_key(...)` and `toon_has_at(...)` for
  compact existence checks on objects and arrays
- `toon_defaults`: uses `toon_get_*_or(...)` helpers to read scalar values with
  typed fallback defaults
- `cost_annotations`: shows validated `@cost` budgets in both `ops` and `ms`
  forms without changing the shared backend
- `contract_layouts`: shows `@pre`, `@post`, and `@cost` surviving realistic
  blank-line and comment-separated layouts before a function declaration
- `task_helpers`: shows the compact Aether task/thread alias surface along with
  the `has_ai()` capability probe, while still respecting `fx` around effectful
  worker launches
- `ai_helpers`: shows the compact `ai_chat(...)` alias and the Aether AI
  capability probes without requiring a live call during the example run
- direct handle helpers such as `toon_key`, `toon_at`, `toon_len`,
  `toon_text_value`, `toon_int_value`, and `toon_free` lowered straight onto
  yyjson builtins
- simple variable-based `toon_parse(...)` flows when a local `Text` or `TOON`
  binding can still be resolved to a literal payload by the frontend rewrite
  layer
- `type Name { ... }` blocks with `field: Type;` members lowered onto the
  shared Rea class/object model
- compact object initialization with `let value: Type = Type { field: expr, ... };`
  lowered onto `new Type()` plus field assignments
- `self` as the Aether source spelling for the current object inside `type`
  methods, lowered onto the shared backend's `myself`
- `@pre` and `@post` lowered into runtime guards
- contract annotations now have frontend attachment checks too: `@pre`,
  `@post`, `@pure`, and `@cost` must all decorate the next function
- `@cost` is now frontend-validated: it must be a positive integer budget with
  an optional unit such as `ns`, `us`, `ms`, `s`, `op`/`ops`, or `step`/`steps`

The backend remains the shared PSCAL compiler and VM.
