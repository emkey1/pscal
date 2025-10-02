# Shell front-end regression tests

Run the suite after building the project:

```sh
python3 shell_test_harness.py
```

Use `--list` to enumerate available cases or `--only <id>` to filter by test id
substring. Test definitions live in `tests/manifest.json` and reuse the example
programs under `Examples/psh/`.
