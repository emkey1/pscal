# exsh front-end regression tests

Run the suite after building the project:

```sh
python3 exsh_test_harness.py
```

Use `--list` to enumerate available cases or `--only <id>` to filter by test id
substring. Test definitions live in `tests/manifest.json` and reuse the example
programs under `Examples/exsh/`.

For PTY-driven interactive Ctrl-C/Ctrl-Z regressions against the iOS-host
`exsh` build, run:

```sh
Tests/run_exsh_ios_interactive_signal_tests.sh
```

or to keep the standard iOS host suite and include interactive tests in one run:

```sh
RUN_INTERACTIVE_SIGNAL_TESTS=1 Tests/run_exsh_ios_host_tests.sh
```
