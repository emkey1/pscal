# Exsh Scope Verification

This package provides a manifest-driven harness for validating scoping
behaviour in the exsh front end. Each test executes a small shell
snippet under `exsh`, optionally mirrors the same snippet under `bash`, and
verifies that the observed results are equivalent.

## Running the tests

```bash
python3 exsh_scope_test_harness.py                # run all tests
python3 exsh_scope_test_harness.py --list         # list available cases
python3 exsh_scope_test_harness.py --only local   # run matching subset
```

By default the harness expects the `exsh` executable in `build/bin/exsh` and
compares parity with `/bin/bash --noprofile --norc`. These can be adjusted with
`--cmd` and `--bash-cmd` respectively.

CSV summaries are written to `out/latest.csv` for quick diffing.
