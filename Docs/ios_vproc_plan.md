# iOS Virtual Process Emulation Plan (clean-room, iSH-inspired)

Goal: keep exsh interactive on iOS while background tasks run, by giving each pipeline stage a private, virtual fd table/tty and synthetic pid state—mirroring iSH’s model without importing GPL code.

## Milestones and Tests

### 1) Scaffolding & API surface
- Add `ios/runtime/vproc.{h,c}` with clean-room vproc API:
  - creation/destruction, per-vproc fd table (dup/dup2/close/open/pipe), stdio slots (stdin/out/err), and synthetic pids.
  - lightweight tty emulation backed by pipes + winsize metadata (no host openpty).
  - TLS guard so threads can opt into vproc fd translation.
- Tests: unit tests under `Tests/ios_vproc/` covering fd dup/dup2/close, pipe, stdio capture, and winsize propagation.

### 2) FD translation shims
- Add read/write/dup/dup2/close/open/lseek/fstat wrappers that consult the active vproc fd table when the TLS guard is set, falling back to real syscalls otherwise.
- Wire shell/exsh to use these wrappers (macro or inline indirection) on iOS builds.
- Tests: targeted shims tests validating translation and fallback; ensure real fds untouched when vproc inactive.

### 3) Pipeline integration (iOS)
- Replace the iOS in-process pipeline/background path to:
  - Create a vproc per stage.
  - Apply redirections inside the vproc table instead of dup2 on real fds.
  - Connect pipeline stages using vproc pipes.
  - Run builtins with the TLS vproc guard enabled so IO goes through the vproc table.
- Tests: shell-level harness that backgrounds `clike ...simple_web_server` and confirms prompt remains responsive; pipeline smoke with redirects; ensure `wait`/`jobs` observe synthetic states.

### 4) Job control & signals
- Synthetic pid/pgid records; map `wait`/`fg`/`bg`/`kill` onto vproc handles.
- Implement minimal signal flags (INT/TSTP/TERM) delivered via shell commands to vproc threads.
- Add synthetic `ps` view that lists vproc-backed tasks (and ensure `kill`/friends operate on synthetic pids, not host pids).
- Tests: background job cancel/fg/bg scenarios; SIGINT propagation to a running builtin.

### 5) Polish & docs
- Trim logging/noisy fallbacks; document the clean-room approach, limitations, and test matrix.
- Tests: rerun relevant exsh threading/task demos on iOS build; regression check that macOS/Linux paths untouched.

## Notes
- No GPL code is imported; behaviour is modelled on iSH’s design but implemented from scratch.
- Host stdio (fd 0/1/2) must never be dup2’d by background tasks; all redirections stay inside vproc tables.
- Keep the host PTY only for the foreground shell; background tasks use vproc tty pipes.
- iOS/iPadOS forbid fork/exec and opening PTYs/ttys from apps; all process emulation must stay within the synthetic vproc/thread layer.

## Progress
- [x] Milestone 1 (scaffolding, fd tables, unit coverage)
- [x] Milestone 2 (syscall shims + translation tests)
- [x] Milestone 3 (pipeline integration & background redirs; iOS pipelines now use vproc pipes/redirs and keep the prompt live)
- [x] Milestone 4 (job control, signals, lps/kill on synthetic PIDs; stable job IDs, `%N` kill, labeled `lps`, synthetic wait/fg/bg)
- [ ] Milestone 5 (polish/docs)

Active focus:
- Tighten Ctrl+C/Z parity with iSH: ensure foreground vproc pipelines interrupt/stop immediately (no deferred stop until completion).
- Harden fg/bg/wait state transitions in virtual TTY mode so the shell never recurses/crashes when contexts swap.
- Add regression coverage for `kill %N`/Ctrl+Z behaviour (job-number stability tests for vproc IDs now exist).
- Trim remaining noisy logging and document iOS-only limitations (no fork/exec/pty).
- Open issues (to avoid repeats):
  - `exsh testjobs`: `%N` kills are sent but targeted jobs remain running; likely signal delivery/handling in vproc background workers rather than job-spec parsing. Need to verify vprocKillShim delivery and worker threads honoring signals.
  - Background worker vprocs still end up sharing/retaining host job IDs unexpectedly after kills; ensure reaping and state updates close running slots.
