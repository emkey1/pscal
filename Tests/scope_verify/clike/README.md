# C-Like Scope Conformance Harness

This package mirrors the manifest-driven regression suite used for the Rea
front end, but targets the C-like compiler/runtime. The harness verifies block
and function scoping, constant visibility, header inclusion, hoisting
behaviour, name resolution, and an integration scenario that mixes several of
those features.

## Running the harness

- Ensure a C-like compiler/runtime binary is available (defaults to
  `build/bin/clike`).
- Invoke the harness from the repository root:
  ```sh
  python3 Tests/scope_verify/clike/clike_scope_test_harness.py \
      --manifest Tests/scope_verify/clike/tests/manifest.json
  ```
- Key flags:
  - `--cmd "<command template with {source}>"` to point at a different
    compiler.
  - `--only PATTERN` to filter tests by substring.
  - `--list` to preview selected tests without executing.
  - `--seed N` to drive deterministic randomised variants (default `1337`).
  - `--update` to materialise `.cl` fixtures under
    `Tests/scope_verify/clike/tests/<category>/` alongside generated helpers.
  - `--out-dir PATH` to redirect generated snippets and reports
    (`out/report.csv`, failure repros under `out/min/`).

## Adding or editing tests

1. Edit `Tests/scope_verify/clike/tests/build_manifest.py` to add or update entries.
   The helper normalises indentation and writes `manifest.json` when run.
2. Optionally add supporting header files via the `files` array on a test
   entry.
3. Regenerate the manifest:
   ```sh
   python3 Tests/scope_verify/clike/tests/build_manifest.py
   ```
4. Run the harness (ideally with `--only` while iterating) and refresh
   materialised fixtures with `--update` if you want checked-in `.cl` files to
   match the manifest versions.

Each manifest entry specifies:
- `id`, `name`, `category`, `description`
- `code` (the primary snippet; supports `{{placeholder}}` templating)
- `expect` (`compile_ok`, `compile_error`, `runtime_ok`, `runtime_error`)
- Optional `expected_stdout`, `expected_stderr_substring`
- Optional `placeholders` for seeded randomisation, and `files` for auxiliary
  headers

## Command template hints

The harness expects `{source}` in the command template and normalises outputs
before comparison. Examples:
- Release build: `--cmd "../../build/bin/clike {source}"`
- Compile-only check: `--cmd "../../build/bin/clike --no-run {source}"`

## Assumptions and notes

The suite encodes several scoping assumptions for the C-like language:
- **Headers**: `#include "path"` brings declarations and definitions into the
  translation unit. Conflicting definitions should be diagnosed.
- **Functions**: Parameters and locals shadow outer bindings without mutating
  them. Functions must be declared (or forward-declared) before use unless
  hoisting via prototypes applies.
- **Blocks & loops**: Lexical blocks introduce new scopes; loop indices do not
  leak after the loop.
- **Constants**: `const` declarations remain immutable after initialisation and
  follow normal scoping rules.
- **Hoisting**: Function declarations are hoisted, including prototypes for
  mutual recursion. Variables and constants are not.
- **Output helpers**: The tests use `printf` for deterministic output
  formatting.

If any of these assumptions diverge from the implementation, adjust the
manifest entries and document the decision inline. The integration test
(`integration_scope_import_shadow_mix`) exercises header inclusion, nested
blocks, and shadowing simultaneously to catch cross-feature regressions.

## Artefacts

- Harness: `Tests/scope_verify/clike/clike_scope_test_harness.py`
- Manifest: `Tests/scope_verify/clike/tests/manifest.json`
- Generator helper: `Tests/scope_verify/clike/tests/build_manifest.py`
- Category directories with `.gitkeep` placeholders for optional materialised
  fixtures
- Generated reports: `Tests/scope_verify/clike/out/report.csv`, repro snippets in
  `Tests/scope_verify/clike/out/min/`

Before publishing changes, run the harness (full suite and any targeted
subsets) and include refreshed outputs if expectations change.
