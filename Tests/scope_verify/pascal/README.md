# Pascal Scope Conformance Harness

This harness mirrors the scope verification utilities used for the other
front-ends.  It executes small Pascal programs, captures their behaviour and
compilation results, and verifies that variable bindings obey the expected
scope rules.

## Usage

```
python3 pascal_scope_test_harness.py --help
python3 pascal_scope_test_harness.py --list
python3 pascal_scope_test_harness.py --only function_scope
```

The harness defaults to running the `build/bin/pascal` front-end produced by the
CMake build.  Use `--cmd` to override the command template if required.

Test definitions live in `tests/manifest.json`.  Each entry supplies the
Pascal source snippet alongside the expected outcome (`runtime_ok`,
`compile_error`, etc.) and optional output assertions.

## Running the harness

- Ensure a Pascal compiler/runtime binary is available (defaults to
  `build/bin/pascal --no-cache`).
- Invoke the harness from the repository root:
  ```sh
  python3 Tests/scope_verify/pascal/pascal_scope_test_harness.py \
      --manifest Tests/scope_verify/pascal/tests/manifest.json
  ```
- Key flags:
  - `--cmd "<command template with {source}>"` to point at a different
    compiler command (for example, with extra diagnostics).
  - `--only PATTERN` to filter tests by substring.
  - `--list` to preview selected tests without executing.
  - `--seed N` to drive deterministic randomised variants (default `1337`).
  - `--update` to materialise `.pas` fixtures under
    `Tests/scope_verify/pascal/tests/<category>/` alongside generated helpers.
  - `--out-dir PATH` to redirect generated snippets and reports
    (`out/report.csv`, failure repros under `out/min/`).

## Adding or editing tests

1. Edit `Tests/scope_verify/pascal/tests/build_manifest.py` to add or update entries.
   The helper normalises indentation and writes `manifest.json` when run.
2. Regenerate the manifest:
   ```sh
   python3 Tests/scope_verify/pascal/tests/build_manifest.py
   ```
3. Run the harness (ideally with `--only` while iterating) and refresh
   materialised fixtures with `--update` if you want checked-in `.pas` files to
   match the manifest versions.

Each manifest entry specifies:
- `id`, `name`, `category`, `description`
- `code` (the primary snippet)
- `expect` (`compile_ok`, `compile_error`, `runtime_ok`, `runtime_error`)
- Optional `expected_stdout`, `expected_stderr_substring`
- Optional `files` for auxiliary units or include files
- Optional `placeholders` for seeded randomisation

## Pascal scoping assumptions exercised

- **Routine scope:** Procedure/function parameters and locals shadow outer
  bindings without mutating them. Nested routines capture enclosing variables.
- **Constant scope:** Constants declared in a block are visible only within that
  block; inner declarations may shadow outer ones.
- **Type scope:** Type declarations follow block scope. Local type aliases do
  not leak outside their defining routine.
- **Block usage:** `begin ... end` blocks serve as statement groupings; new
  declarations must appear in the surrounding block's declarative part.
- **Integration:** The final test checks that nested routines, shadowed
  constants, and local types all cooperate as expected.

If implementation details diverge from these behaviours, adjust the manifest and
document the decision inline so future contributors understand the expectations.

## Artefacts

- Harness: `Tests/scope_verify/pascal/pascal_scope_test_harness.py`
- Manifest: `Tests/scope_verify/pascal/tests/manifest.json`
- Generator helper: `Tests/scope_verify/pascal/tests/build_manifest.py`
- Generated reports: `Tests/scope_verify/pascal/out/report.csv`, repro snippets in
  `Tests/scope_verify/pascal/out/min/`

Run the harness (full suite or focused subsets) before publishing changes to
confirm that scoping behaviour matches the Pascal front end's specification.
>>>>>>> e062294 (Good stuff)
