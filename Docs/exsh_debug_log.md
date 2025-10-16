# exsh Debug Log

This document records troubleshooting steps and findings while investigating the shellbench regressions for the exsh front end.

## Session 2025-10-16
- Initialized log document for ongoing troubleshooting.
- Plan: configure and build exsh via CMake to enable manual reproduction of shellbench scenarios.
- Ran `cmake -S . -B build` (SDL auto-disabled) to generate build system.
- Built `exsh` via `cmake --build build --target exsh`.
- Reviewed shellbench `sample/assign.sh` via GitHub to understand `@begin/@end` instrumentation macros.
- Downloaded the `shellbench` harness script locally to inspect macro expansion (`generate_*_begin_helper`).
- Plan: reproduce `assign.sh` benchmark using local harness and exsh binary to capture runtime errors.
- Ran `/tmp/shellbench` with `SHELLBENCH_SHELLS=./build/bin/exsh` against `sample/assign.sh`; confirmed '?' results for multiple cases while others succeed (e.g., `local var`).
- Plan: source shellbench helpers to capture expanded benchmark code for failing case (`positional params`).
- Attempted to rerun benchmark with `-e` flag for error output; initial run hung (interrupted) — need to retry with reduced timing.
- Retried with `SHELLBENCH_BENCHMARK_TIME=1`, `SHELLBENCH_WARMUP_TIME=0`, and `-e`; still saw '?' for `variable`/`local var` tests without additional error output.
- Used `__SOURCED__=1 . /tmp/shellbench` trick to dump translated code for `positional params`; observed wait-loop with `kill -HUP "$MAIN_PID"` handshake.
- Attempted to background-run extracted benchmark script manually to capture FD3 output; shell session dropped (need to retry carefully).
- Created helper harnesses (`run_variable_bench.sh`, etc.) to orchestrate HUP/TERM signals; confirmed `__finished` handler fires and writes counts when FD3 targets a file.
- Instrumented benchmark code with debug logs showing handshake progression and large iteration counts (~28k) during manual runs.
- Observed worker processes exiting with status 1 under manual control, matching shellbench's reliance on the EXIT trap output rather than process status.
- Built debugging variant of `shellbench` to redirect FD3 to temporary files; saw stable counts for some tests while others still produced '?' (e.g., `local var`).
- Wrapped exsh with a tee on FD3 to capture raw counts during stock shellbench runs; captured four `1` outputs despite shellbench reporting '?', suggesting the harness receives minimal iteration counts.
- Multiple attempts to call `bench` directly from a sourced shellbench environment caused the interactive shell session to hang, indicating extra care needed when debugging inside the harness.
- Noticed `echo hi >&3` continued to print to stdout when FD 3 was closed; traced this to `shellEnsureExecRedirBackup` reusing the same descriptor as the redirection source.
- Updated redirection backup logic to duplicate into a descriptor past the avoid range, preventing collisions with the source FD and restoring expected `>&` semantics.
- Rebuilt exsh and confirmed direct tests (`echo hi >&3` with closed FD) now error out; reran `assign.sh` benchmarks to verify `variable`/`positional params` counts recover (remaining anomalies isolated to `local`/`typeset` cases pending follow-up).
