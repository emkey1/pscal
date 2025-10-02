# Rea Library Test Suite

This directory hosts an opt-in test harness for the Rea standard library
modules that ship with this repository.  The suite exercises every exported
function from the `crt`, `datetime`, `filesystem`, `http`, `json`, and
`strings` modules.

The tests are intentionally kept out of the main regression suite because they
require optional runtime features (HTTP and yyjson built-ins) and network
connectivity.  They can be run manually whenever the Rea front end is built and
available.

## Prerequisites

* Build the Rea compiler so that `build/bin/rea` exists, or set the
  `REA_BIN` environment variable to point at an alternative executable.
* Ensure the yyjson extended built-ins are available if you want to exercise the
  JSON tests.  The suite will skip those checks automatically when yyjson is
  missing.

## Running the suite

From the repository root:

```bash
  python3 Tests/libs/rea/run_tests.py
```

The helper script starts a small HTTP server for exercising the networking
helpers, wires up the import path so the bundled libraries are visible, and then
launches the Rea program that performs the assertions.  Before running the
program it inspects the compiler's optional extended built-ins via
``--dump-ext-builtins`` and exports the results through
``REA_TEST_EXT_BUILTINS`` (along with ``REA_TEST_HAS_YYJSON``) so JSON checks can
be skipped automatically when yyjson support is not present.  At the end of the
run a summary is printed showing the number of passing, failing, and skipped
checks.  The script exits with a non-zero status if any checks fail.
