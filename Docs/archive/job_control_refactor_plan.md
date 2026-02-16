# Job Control Refactor Plan

## Objectives
- Deliver reliable, bash-compatible job control for ctrl-c/ctrl-z, fg/bg, jobs, and disown, while preserving iOS/Mac Catalyst constraints.
- Centralize process-group, signal, and terminal ownership handling so pipelines, builtins, and external commands share one path.
- Keep behavior predictable on platforms without full job control (iOS/Catalyst shims) with graceful degradation and clear user feedback.
- Reduce duplication between vproc shims and POSIX syscalls; isolate platform capability checks and fallbacks.

## Pain Points (today)
- Foreground/tty ownership is scattered: tcsetpgrp/vprocSetForegroundPgid calls live in multiple helpers; stdio restore is partially duplicated in pipeline/bg code.
- Job tracking mixes pipeline context with ad-hoc arrays; stopped/running transitions and pgid propagation are not modeled as a state machine.
- Signal handling (SIGINT/SIGTSTP) is intertwined with word expansion and builtins; ctrl-c/ctrl-z behavior differs between interactive, pipeline, and bg paths.
- Platform differences (no real tcsetpgrp on iOS, thread-pgid semantics in vproc) are addressed piecemeal, making regressions likely.

## Design Overview
### Capability Matrix
- Define per-platform capability flags (e.g., HAS_JOB_CONTROL, HAS_TCSETPGRP, HAS_REAL_PGID, HAS_STOP_SIGNALS) computed once at startup.
- Wrap all terminal ownership operations through a single API that chooses tcsetpgrp/killpg or vprocSetForegroundPgid/vprocKillShim based on capabilities.

### Core Types
- `ShellJob`: pgid, vector of pids, command summary, state (New → Running(fg/bg) → Stopped → Continued → Done), last_status, and tty/stdio snapshot handles.
- `JobControlContext`: current fg job id, shell pgid, tty fd, capability flags, and saved shell termios.
- `JobWaitContext`: used by wait/fg/bg to manage waits, continuation, and signal delivery consistently.

### API Surface
- `jobControlInit()` / `jobControlEnsure()` sets tty, pgid, capabilities, ignores TTIN/TTOU, and records shell termios.
- `jobControlAssignPipeline(ctx)` attaches pgid/stdio snapshot to a pipeline before launch; `jobControlEnterForeground(job)` and `jobControlRestoreShell()` own tty transitions.
- `jobControlHandleSignal(signo)` centralizes ctrl-c/ctrl-z dispatch: sends SIGINT/SIGTSTP to fg job (pgid), updates state, and triggers prompt resume.
- `jobControlWait(job, opts)` waits with WUNTRACED/WCONTINUED where supported, updates state machine, and prints suspended/continued lines.
- Builtin helpers (`fg`, `bg`, `jobs`, `disown`) call these APIs rather than touching pgid/pids directly.

### Signal Handling
- Install shell-level handlers that enqueue pending signals and let the dispatcher route them based on current fg job and capabilities.
- On platforms without stop support, translate ctrl-z to a no-op with a user-facing warning, leaving shell responsive.
- Ensure traps and interactive key handling share the same pending-signal path to avoid divergence.

### Pipeline Integration
- Pipeline creation populates a `ShellJob` before launch (pgid + pids as they spawn).
- Foreground pipelines: save shell stdio/termios, set pgid/foreground, run, wait via `jobControlWait`, restore shell stdio/tty, and update status.
- Background pipelines: register job, retain stdio snapshot for future fg, ensure redirections persist for bg workloads (including iOS in-process paths).

### Platform Notes
- POSIX (desktop): full tcsetpgrp/killpg; supports STOP/CONT; use pgid for all signal delivery.
- iOS/Mac Catalyst: no real tcsetpgrp; vproc shims emulate pgid/tty handoff best-effort; STOP may be disabled—ctrl-z becomes logical stop/continue with messaging; ensure no SIGTSTP leaks to host app.
- Host-test shims: allow in-process pipelines to exercise logic without real pgids; capability flags gate behavior.

## Implementation Phases
1) Capability layer: introduce flags and wrapper functions for pgid/tty/signal ops; no behavior change yet.
2) Job structs/state machine: add `ShellJob`/`JobControlContext` updates; route existing job tracking through them with compatibility shims.
3) Foreground/tty unification: refactor pipeline fg/bg paths to use `jobControlEnterForeground/RestoreShell`; remove ad-hoc tcsetpgrp calls.
4) Signal dispatch consolidation: central handler for ctrl-c/ctrl-z; adjust word expansion/builtin handling to consume queued signals; ensure traps run post-dispatch.
5) Builtin rewiring: migrate fg/bg/jobs/disown/kill to the new API; ensure consistent status/reporting and pgid usage.
6) Testing: expand exsh suites with ctrl-c/ctrl-z, fg/bg resume, stopped job reporting, and platform-specific (iOS-capability-limited) expectations; add integration cases covering pipelines with redirections.

## Testing Plan
- Automated: exsh parity for fg/bg/jobs/disown; ctrl-c in pipelines; ctrl-z stop/resume; signal delivery to external commands; redirection persistence after stop/fg. Platform-gated variants for iOS/Catalyst vs POSIX.
- Manual: long-running bg jobs (e.g., simple_web_server) with redirection, interrupt/stop/resume cycles; ensure prompt and tty ownership recover cleanly.
- Regression: rerun existing bash_parity_* and pipeline suites to verify no ID/pgid regressions.
