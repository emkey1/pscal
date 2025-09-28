# Pascal Library Test Suite

This directory hosts an opt-in test harness for the Pascal standard library
units that ship with this repository. The program exercises the helper units in
`lib/pascal` such as `Base64`, `CalculateArea`, `CRT`, `MathLib`, `myLib`,
`StringUtil`, and `SysUtils` so they can be validated outside of the main
regression suite.

The suite lives outside of `Tests/run_pascal_tests.sh` because it depends on the
installed library tree and optional runtime features. It can be run manually
whenever the Pascal front end is built and available.

## Prerequisites

* Build the Pascal compiler so that `build/bin/pascal` exists, or set the
  `PASCAL_BIN` environment variable to point at an alternative executable.
* Ensure the Pascal library directory is accessible. The helper script will
  configure `PASCAL_LIB_DIR` automatically, but any existing value is preserved
  to allow testing alternate installations.

## Running the suite

From the repository root:

```bash
python3 etc/tests/pascal/run_tests.py
```

The helper script configures the import path so the bundled libraries are
visible, creates a temporary directory for any file-based checks, and then
launches the Pascal program that performs the assertions. Before running the
suite it queries the compiler for the set of optional extended built-ins that
were compiled in via ``--dump-ext-builtins`` and exposes the results through
``PASCAL_TEST_EXT_BUILTINS`` (and a convenience flag for yyjson support) so the
Pascal test program can skip checks that rely on features not present in the
current build. A summary is printed showing the number of passing, failing, and
skipped checks. The script exits with a non-zero status if any checks fail.
