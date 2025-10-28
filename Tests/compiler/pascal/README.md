# Pascal Compiler Regression Suite

This suite exercises compile-time behaviour of the Pascal front end. Each case
in `manifest.json` runs the compiler in non-cached mode against a small source
file and asserts whether compilation succeeds or produces a targeted semantic
error.

## Adding Tests

1. Place the Pascal source under `cases/`.
2. Add an entry to `manifest.json` with:
   - a unique `id`,
   - the relative `path` to the source,
   - `expect` set to `compile_success` or `compile_error`, and
   - optional `stdout_substring`/`stderr_substring` checks.
3. Run `python3 run_compiler_tests.py` (from this directory) or
   `Tests/run_pascal_tests.sh` to execute the suite.

The closure fixtures ensure the new escape analysis emits
`closure captures a local value that would escape its lifetime.` when a nested
procedure that closes over locals attempts to escape its defining scope, while
allowing non-capturing nested procedures to compile and execute within their defining scope.
