# CLike Library Test Suite

This directory hosts an opt-in test harness for the CLike standard library
modules that ship with this repository. The suite exercises the helpers in
`lib/clike`, mirroring the checks that exist for the Rea standard library so the
CLike front end can be verified independently.

The tests are kept separate from the main regression suite because they require
optional runtime features (HTTP built-ins and yyjson) and network connectivity.
They can be run manually whenever the CLike front end is built and available.

## Prerequisites

* Build the CLike compiler so that `build/bin/clike` exists, or set the
  `CLIKE_BIN` environment variable to point at an alternative executable.
* Ensure the CLike library directory is accessible. The helper script will set
  `CLIKE_LIB_DIR` automatically when running from the repository root.

## Running the suite

From the repository root:

```bash
python3 etc/tests/clike/run_tests.py
```

The helper script starts a small HTTP server for exercising the networking
helpers, prepares a temporary directory along with a JSON fixture, and launches
the CLike program that performs the assertions. Before executing the test
program it inspects the compiler's optional extended built-ins via
``--dump-ext-builtins`` and exports the results through
``CLIKE_TEST_EXT_BUILTINS`` (plus a ``CLIKE_TEST_HAS_YYJSON`` convenience flag)
so JSON checks can be skipped automatically when yyjson isn't available. At the
end of the run a summary is printed showing the number of passing, failing, and
skipped checks. The script exits with a non-zero status if any checks fail.
