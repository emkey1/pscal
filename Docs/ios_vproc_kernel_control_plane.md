# iOS vproc Kernel Control Plane

## Purpose

This document tracks the architecture for moving iOS `vproc` signal/job-control
coordination toward a Unix-like model while staying safe in an app-hosted
runtime (no host-process-wide signal side effects).

## Why This Exists

iOS does not provide normal process groups + terminal job control for our
synthetic `vproc` tasks. We emulate this in userspace. As the number of
threads/tasks grows, direct ad-hoc signal dispatch from multiple threads causes
ordering races and re-entrancy hazards.

The "kernel" vproc/thread is the natural place to centralize control-plane
events.

## Current Architecture (Phase 1 + Phase 2)

### Data Plane

- Worker/shell threads execute commands and read/write PTYs/FD shims directly.
- `vproc` task state (pid/pgid/sid/signal state) remains in `gVProcTasks`.

### Control Plane

- A dedicated kernel thread exists (`vprocKernelThreadMain`).
- It now processes queued control-signal events (`SIGINT`/`SIGTSTP`) from a
  serialized queue under `gKernelThreadMu`.
- Foreground handoff (`sid`/`fg_pgid` sync), `SIGCONT` resume dispatch, and
  synthetic `SIGCHLD` aggregation are also queued through the kernel control
  plane event worker.
- Session input reader queues control-signal events to the kernel thread when
  the shell is not foreground; it still has safe inline fallback if queueing is
  unavailable.
- Shell-foreground routing of `^C/^Z` to shell input bytes is additionally
  gated by per-shell prompt-read state stored in `gVProcTasks` via
  `vprocSetShellPromptReadActive(pid, active)`/`vprocShellPromptReadActive(pid)`.
  This avoids cross-session bleed-through where one prompt-active shell could
  cause another session's control bytes to be misrouted as plain input.
- VM-level runtime interrupt consumption is now foreground-scoped on iOS:
  background/non-foreground vprocs no longer consume/clear shared runtime
  `SIGINT` flags before the foreground frontend VM can observe them.
- In-process stage teardown is now stop-aware: a mapped stop status
  (`128 + SIGTSTP/SIGSTOP/SIGTTIN/SIGTTOU`) is no longer force-converted to an
  immediate vproc exit/discard.
- For stopped in-process stages, teardown unregisters the shell thread from the
  stage task entry before preserving stop state so later signal routing does not
  bleed back into the shell thread identity.
- `vprocCommandScopeEnd()` (used by scoped smallclue applets and shell
  integration wrappers) is now also stop-aware: stop-like statuses no longer
  force exit/discard, including paths that temporarily set `stop_unsupported`.
- `PSCALRuntimeSendSignal()` now injects virtual control signals for
  `SIGINT`/`SIGTSTP`/`SIGTERM` through `vprocRequestControlSignal()`, which
  queues into the kernel control plane and avoids host fatal signal delivery.
- Runtime/session control dispatch now supports explicit shell targeting via
  `vprocRequestControlSignalForShell(shell_pid, sig)` so UI-originated `^C/^Z`
  can route correctly even when called from non-runtime worker threads that do
  not have `vproc` session stdio TLS active.
- Runtime/session control dispatch now also supports explicit session targeting
  via `vprocRequestControlSignalForSession(session_id, sig)`, which resolves
  foreground pgid through the session PTY map and then dispatches virtual
  control signals.
- `PSCALRuntimeSendSignal()` now accepts active per-session contexts (shell tab
  sessions created through `PSCALRuntimeCreateShellSession`) and no longer
  requires `s_runtime_active` to be true before dispatching `SIGINT/SIGTSTP`.
- `PSCALRuntimeSendSignal()` now attempts control dispatch in this order:
  1) session-id scoped (`vprocRequestControlSignalForSession`)
  2) explicit shell-pid scoped (`vprocRequestControlSignalForShell`)
  3) thread-local/global shell scoped (`vprocRequestControlSignal`)
  This reduces dropped/misrouted `^C/^Z` when TLS shell state is unavailable.
- Session-input `^C/^Z` dispatch now avoids VM-global runtime fallbacks in the
  hot path. If foreground/session dispatch cannot resolve a target, the control
  byte is preserved in the session input queue instead of being silently dropped
  or escalated to app-wide runtime interrupt/suspend callbacks.
- Session-input shell-hint resolution is now session-local (`ctx->shell_pid` /
  `session->shell_pid`) and no longer falls back to global shell pid lookup
  from that reader thread, reducing cross-session bleed.
- Added explicit bridge entrypoint `PSCALRuntimeSendSignalForSession(session_id, signo)`.
  Swift local-shell handlers now target the active session directly and only
  fall back to injecting raw control bytes when virtual dispatch reports no
  delivery.
- Control-key delivery is intentionally split:
  - local shell/frontend tabs emit explicit runtime control signals (`SIGINT`,
    `SIGTSTP`) through `PSCALRuntimeSendSignal()`.
  - remote SSH tabs continue to send literal control bytes (`0x03`, `0x1A`)
    through PTY input to preserve remote TTY semantics.
- UI/runtime prompt refresh paths no longer inject synthetic user input bytes
  (e.g. space + DEL "prompt kick"). Prompt refresh now uses non-input UI
  refresh/focus flow so startup-command sessions (including `ssh <host>`) do
  not receive unintended bytes during password reads.
- iOS key input normalization now treats raw control bytes (`0x01..0x1A`) and
  `Ctrl+<letter>` as equivalent for control-signal routing, so `^C/^Z`
  consistently invoke the configured interrupt/suspend handlers before
  optionally falling back to literal byte injection.
- `TerminalWindow` now has a global hardware-key fallback for `Ctrl+C`/`Ctrl+Z`
  that routes to the currently selected tab, so control handling still works
  when WKWebView/input-focus churn prevents per-view key handlers from seeing
  the keypress.
- Runtime `SIGINT`/`SIGTSTP` request routing now treats foreground delivery as
  successful only when `vprocKillShim(-fg_pgid, sig)` succeeds; failed fg
  routing no longer silently drops control signals.
- `SIGTSTP` fallback now prefers virtual shell-pgid delivery before setting the
  VM suspend flag, reducing accidental frontend exits when a fg pgid reference
  is stale.
- Smallclue VM-invoked scoped applets now use non-inherited pgids and set
  foreground control to the scope's effective pgid, reducing shell-group signal
  bleed.
- For `stop_unsupported` tasks, `SIGTSTP` is now recorded as pending so
  cooperative frontends can surface status `128 + SIGTSTP` and preserve stop
  semantics instead of silently dropping stop intent.

### Known Gaps (Current)

- No known open Phase 1 regressions in the ios-host interactive suite
  (`interactive_ctrl_c_prompt`, `interactive_ctrl_c_sleep`,
  `interactive_ctrl_z_sleep`, `interactive_ctrl_c_watch`).
- Remaining work is robustness/stress and deeper centralization of transition
  ownership (Phase 3/4).

### Shell-Foreground Rule

- If shell pgid is foreground, control bytes (`^C`, `^Z`) are routed as shell
  input bytes (prompt/editor semantics), not forced as synthetic process-group
  signals.
- If a non-shell foreground job owns the terminal, control bytes are translated
  to control-signal events and dispatched to foreground pgid.

## Invariants

- Never raise host `SIGINT`/`SIGTSTP` on iOS app threads.
- Never call host `kill()` fallback for iOS virtual control/job-control
  signals (`SIGINT`/`SIGQUIT`/`SIGTSTP`/`SIGTERM`/`SIGCONT`/`SIGSTOP`/`SIGTTIN`/`SIGTTOU`).
- Kernel control-plane queue processing must be single-threaded.
- Foreground job selection for control-signal dispatch is computed at dispatch
  time (not enqueue time) to reduce stale-state races.
- Queue dispatch fallback path must preserve behavior if kernel worker is not
  ready.

## Completed in This Pass

- Kernel thread converted from `pause()` loop to event worker loop.
- Added kernel control-signal event queue.
- Added/validated queued kernel events for:
  - foreground handoff notifications
  - continue-pgid dispatch
  - deferred `SIGCHLD` aggregation delivery
- Session input path now enqueues non-shell-foreground `^C/^Z` dispatches.
- Added tests for:
  - shell-foreground control chars stay in shell input queue.
  - non-shell foreground `^C` dispatches to foreground job and interrupts read.
  - frontend-like foreground groups (`pascal` + `rea`) receive `^Z` as stop
    on all group members, then resume/exit cleanly.
  - `stop_unsupported` frontend-like foreground groups queue pending `SIGTSTP`
    on `^Z` (no forced exit) and still terminate on subsequent `^C`.
  - control-signal dispatch is session-isolated: `^C`/`^Z` in session A does
    not bleed into independent foreground jobs in session B.
- In-process pipeline finish now preserves stopped-stage pids instead of eagerly
  clearing them, so suspended jobs can still be registered as stopped jobs.
- Added explicit regression coverage for scoped command teardown preserving
  `SIGTSTP` stop state.
- Added regression coverage ensuring `stop_unsupported` tasks receive pending
  `SIGTSTP` delivery (cooperative stop), rather than silently discarding stop
  requests.
- Normalized cooperative stop status mapping to `128 + SIGTSTP` (instead of
  hardcoded `148`) so stop detection and job registration are consistent with
  platform signal numbering.
- Restored `WUNTRACED` stop observability for non-`stop_unsupported`
  thread-backed vprocs by removing an over-broad `SIGTSTP` queue-only path.
- Hardened `vprocKillShim` to keep iOS control/job-control signals fully
  virtualized even from non-vproc threads: `SIGINT`/`SIGTSTP`/related job-control
  signals no longer fall back to host `kill()` when no virtual context is active.
- Added regression coverage that `kill(0, SIGINT/SIGTSTP)` through the vproc shim
  does not deliver host process signals when virtual context is absent.
- During command execution on iOS host builds, terminal mode now disables
  `ICANON|ECHO|ISIG` (restored afterward) so `^C/^Z` arrive immediately to the
  session input/control-plane path instead of being line-buffered until newline.
- Smallclue VM integration now applies cooperative stop handling for in-process
  worker-vproc applets so `^Z` yields stoppable shell-job state instead of
  parking worker threads in a non-resumable join path.
- Re-verified:
  - `Tests/run_ios_vproc_tests.sh`
  - `Tests/exsh/exsh_interactive_test_harness.py` (all interactive signal
    scenarios pass).
- Added runtime-origin control-signal dispatch coverage:
  - `runtime_request_ctrl_c_dispatches_to_foreground_job`
  - `runtime_request_ctrl_z_stops_foreground_job`
- Added explicit-shell runtime dispatch coverage:
  - `runtime_request_ctrl_c_dispatches_with_explicit_shell_pid`
  - `runtime_request_ctrl_z_stops_with_explicit_shell_pid`
- Added explicit-session runtime dispatch coverage:
  - `runtime_request_ctrl_c_dispatches_with_explicit_session_id`
  - `runtime_request_ctrl_z_stops_with_explicit_session_id`
- Added session-targeted signal API wiring in iOS app runtime:
  - `ShellRuntimeSession.sendInterrupt/sendSuspend` now call
    `PSCALRuntimeSendSignalForSession(...)` first.
  - `PscalRuntimeBootstrap.sendInterrupt/sendSuspend` now use active
    `sessionId` when present.
  - Both paths inject raw control bytes (`0x03`/`0x1A`) only as fallback when
    explicit session-scoped virtual dispatch reports no delivery.

## Next Phases

### Phase 3: Centralize State Transitions

- Move more stop/cont/exit state transitions behind kernel control-plane events
  to reduce cross-thread lock ordering/races.

### Phase 4: Stress and Determinism

- Add multi-thread stress tests (rapid `^C/^Z`, fg/bg churn, concurrent
  wait/reap) with deterministic assertions on ordering.
