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
