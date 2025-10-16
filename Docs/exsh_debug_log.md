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

## Session 2025-10-17
- Reproduced the intermittent `?` tallies with the upstream `shellbench` harness and noted EXIT traps were skipped whenever `set -e` had marked the run for termination.
- Updated `shellRuntimeExecuteTrap` to snapshot and clear exit/break/continue/errexit flags (plus the current VM state) before invoking a trap body, then merge the trap's results back into the caller so traps run with a clean slate without losing pending exits.
- Adjusted `shellRunSource` so any exit requests signalled by the EXIT trap are folded back into the outer interpreter and the trap's exit status becomes the script's final status.
- Verified the fix with `/tmp/shellbench -e -s /workspace/pscal/build/bin/exsh /tmp/assign.sh`, which now reports consistent counts across all four `assign.sh` benches (e.g. `29,959 / 18,807 / 18,976 / 19,722`).
- Collected additional samples (`/tmp/assign_variable.sh`, `/tmp/assign_local.sh`) to confirm EXIT traps now fire reliably in both `local` and `typeset` variants under `set -e`.

## Session 2025-10-24
- Re-ran shellbench smoke scripts and observed every logical/loop benchmark falling back to `?`, with direct reproducers showing `set -e` terminating on the first failing clause in constructs like `false && body`.
- Traced the regression to `shellUpdateStatus` forcing an immediate `shellRuntimeRequestExit()` for all non-zero statuses, which aborted the VM before the logical short-circuit bytecode could inspect `$?` and skip the right-hand side.
- Introduced a deferred-`errexit` flow: failures mark `errexit_pending` while the VM loop calls a new `shellRuntimeMaybeRequestPendingExit()` helper that only escalates after one iteration. The logical guard (`vmHostShellLastStatus`) clears the pending flag when the failure is intentionally handled, so short-circuit and conditional contexts proceed, but ordinary `set -e` failures still exit immediately.
- Added the VM hook via weak symbols so non-shell frontends continue to link, and verified hand-authored scripts (`set -e; false && echo ok`, `set -e; false || echo ok`, `set -e; if false; then ...`) now match bash semantics.
- Cleaned up the temporary `/tmp/exsh_*` logging that accumulated during earlier diagnostics to remove unnecessary file I/O and warning spam, then rebuilt `exsh` to confirm a clean compile.
- Confirmed the EXIT trap handshake survives the change by running the bench-style harness (`trap 'echo "$count" >&3' EXIT` loop) and checking FD3 captures the expected iteration count.
