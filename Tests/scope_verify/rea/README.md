# Rea Scope Conformance Harness

This package provides a manifest-driven regression suite and a standalone Python harness for validating scope behaviour across the Rea front end. The tests cover lexical blocks, functions, closures, modules/imports, constants, type aliases, pattern bindings, name resolution, hoisting rules, and an end-to-end integration scenario.

## Running the harness

- Ensure a Rea compiler/runtime binary is available (defaults to `build/bin/rea`).
- Invoke the harness from the repository root:
  ```sh
  python3 Tests/scope_verify/rea/rea_scope_test_harness.py \
      --manifest Tests/scope_verify/rea/tests/manifest.json
  ```
- Key flags:
  - `--cmd "<command template with {source}>"` to point at a different compiler.
  - `--only PATTERN` to filter tests by substring.
  - `--list` to preview selected tests without executing.
  - `--seed N` to drive deterministic randomised variants (default `1337`).
  - `--update` to materialise `.rea` fixtures under `Tests/scope_verify/rea/tests/<category>/` alongside generated modules.
  - `--out-dir PATH` to redirect generated snippets and reports (`out/report.csv`, failure repros under `out/min/`).

## Adding or editing tests

1. Edit `Tests/scope_verify/rea/tests/build_manifest.py` to add or update entries. The helper normalises indentation and writes `manifest.json` when run.
2. Optionally add supporting module files via the `files` array on a test entry.
3. Regenerate the manifest:
   ```sh
   python3 Tests/scope_verify/rea/tests/build_manifest.py
   ```
4. Run the harness (ideally with `--only` while iterating) and refresh materialised fixtures with `--update` if you want checked-in `.rea` files to match the manifest versions.

Each manifest entry specifies:
- `id`, `name`, `category`, `description`
- `code` (the primary snippet; supports `{{placeholder}}` templating)
- `expect` (`compile_ok`, `compile_error`, `runtime_ok`, `runtime_error`)
- Optional `expected_stdout`, `expected_stderr_substring`
- Optional `placeholders` for seeded randomisation, and `files` for auxiliary modules

## Command template hints

The harness expects `{source}` in the command template and normalises outputs before comparison. Examples:
- Release build: `--cmd "../../build/bin/rea {source}"`
- Verbose compile-only check: `--cmd "../../build/bin/rea --no-run {source}"`

## Assumptions and open questions

The suite encodes several scoping assumptions that should be confirmed against the evolving Rea specification:
- **Modules & imports**: `module Name { ... }` groups exports; `export` marks public members; `#import "path" as Alias` makes the module available as `Alias`. Ambiguous unqualified references must error.
- **Closures**: Nested functions are legal, capture outer locals by reference, and cannot escape their defining scope when they capture locals (returning such a closure is a compile error).
- **Pattern matching**: `match` expressions bind variables per arm; guards see those bindings; bindings do not leak. Exception handlers use `try`/`catch` with scoped variables.
- **Generics & aliases**: `type alias` introduces a new type name. Generic parameters (`T`) are scoped to their definition and cannot leak.
- **Hoisting**: Function/procedure declarations are hoisted (including mutual recursion with prototypes), but variables and constants are **not**.
- **Output formatting**: `writeln` accepts multiple arguments, printing them in order with default spacing.

If any of these assumptions diverge from the actual implementation, adjust the manifest entries and note the decisions inline. The integration test (`integration_scope_closure_module_mix`) exercises modules, nested closures, and shadowing simultaneously to catch cross-feature regressions.

## Artefacts

- Harness: `Tests/scope_verify/rea/rea_scope_test_harness.py`
- Manifest: `Tests/scope_verify/rea/tests/manifest.json`
- Generator helper: `Tests/scope_verify/rea/tests/build_manifest.py`
- Category directories with `.gitkeep` placeholders for optional materialised fixtures
- Generated reports: `Tests/scope_verify/rea/out/report.csv`, repro snippets in `Tests/scope_verify/rea/out/min/`

Before publishing changes, run the harness (full suite and any targeted subsets) and include refreshed outputs if expectations change.
