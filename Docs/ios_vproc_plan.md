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
- [ ] Milestone 3 (pipeline integration & background redirs)
- [ ] Milestone 4 (job control, signals, lps/kill on synthetic PIDs)
- [ ] Milestone 5 (polish/docs)

Active focus:
- Ensure built-in frontends (clike/pascal/rea/pscalvm/pscaljson2bc/exsh) dispatch inline on iOS so background jobs don’t return 127 and the prompt stays responsive.
- Finish tying shell job-control builtins (fg/bg/wait/kill) and `lps` listings to the synthetic vproc table; shell-facing `kill`/friends must target synthetic PIDs instead of host processes. Kill/wait/fg/bg now use vproc shims for synthetic PIDs, and `%N` jobspecs resolve against synthetic task snapshots.
- Auto-register any untracked vproc tasks into the shell job table so `jobs`/`fg`/`bg` always see synthetic processes even if registration was skipped.
- Ctrl+C/Z on iOS now raise SIGINT/SIGTSTP into the active vproc pipeline; remaining: foreground/background state transitions for Ctrl+Z without a real SIGTSTP stop.
- Ensure vproc kill attempts thread cancellation so blocking syscalls (sleep, etc.) terminate promptly; lps now shows placeholder names for tasks without recorded commands.
- SIGTSTP on iOS now forces a clean exit from the current builtin (sets status 128+SIGTSTP) instead of refusing Ctrl+Z outright, giving a minimal suspend/abort path in virtual TTY mode.
