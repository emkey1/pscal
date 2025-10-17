# Scalar cache integration notes

- Need to reintroduce `ShellScalarCacheEntry` struct/state in `shell_runtime_state.inc` (struct fields + globals).
- Add prototypes/definitions for cache helpers (ensure capacity/find/store/remove/equals) and capture stack helpers.
- Extend `ShellRuntimeState` with capture and cache stats fields; initialise in `gShellRuntime`.
- Reapply `shell_command_utils.inc` changes to use cache + capture helpers (previous diff can be reused).
- After code compiles, run `cmake --build build` and `python3 Tests/run_all_suites.py`.
- Shellbench must be executed manually outside the harness; rely on the previously collected numbers.
