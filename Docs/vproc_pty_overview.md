# Virtual Processes and PTYs in PSCAL (iOS/Catalyst)

This note summarizes how the virtual process (vproc) and pseudo-tty (vpty) layers are wired for the iOS/Catalyst ports and how different executable classes (front-end languages, applets/smallclue, nextvi, etc.) ride on top of them.

## High-level architecture
- **vproc** is a clean-room process abstraction used on iOS/Catalyst when the global syscall shims are active. Each vproc maintains its own fd table, pid/pgid/sid, job-control state, and limited signal delivery. Shimmed libc calls (open/read/write/dup/ioctl/poll/select/kill/waitpid/etc.) route into vproc when a vproc is current on the thread; otherwise they fall back to host syscalls.
- **vpty** is the pseudo-tty stack used by the shell/runtime to provide interactive terminals. It presents master/slave pairs backed by `pscal_fd` objects (see `src/ios/tty/pscal_pty.c` and `pscal_tty.c`) and is exposed to userland as `/dev/ptmx` + `/dev/pts/N` via the vproc device handlers.
- **Session plumbing**: the PSCAL runtime (Swift side) creates a shell session (exsh) per tab. On startup it sets environment, builds a `VProcSessionStdio` bundle (stdin/stdout/stderr, pty master/slave), and registers output handlers. The session input thread feeds keystrokes into the vpty master; output is drained from the master and pushed to the UI. Foreground pgid tracking is maintained so ^C/^Z can be dispatched to the active job.

## Execution model by executable class
- **Front-end languages (clike, pascal, rea)**: launched via the exsh front-end. Each command spawns a vproc (via `vprocCommandScopeBegin`) inheriting the session stdio. The vproc fd table maps STDIN/STDOUT/STDERR to the session pty slave; job-control metadata (pid/pgid/sid/fg_pgid) is tracked in `gVProcTasks`. Signals (`kill`, `tcsetpgrp`, etc.) are routed through the vproc tables.
- **Applets / smallclue binaries**: built as standalone tools but still run under exsh. Because the global shims are injected (`-include ios/vproc_shim.h`), their libc calls resolve to vproc shims when a current vproc exists. They inherit the session pty via the same `vprocCommandScopeBegin` path, so job-control and signal delivery match the front-end languages.
- **nextvi/editor (nextvi, built-in editor window)**: runs inside the same session when invoked from exsh. The editor uses the session pty (alt-screen, cursor control) like other curses-style tools. Its syscalls go through the vproc shims as long as a current vproc is active on the thread.
- **Runtime helper binaries (tool runner, PSCAL toolchain utilities)**: when invoked from the Swift runtime (e.g., asset installer or background tasks), a shell context and vproc may be installed explicitly via `PSCALRuntimeSetShellContext`. If no current vproc exists, calls fall back to host syscalls and thus bypass vproc state (no virtual pid/pgid, no vpty).
- **Non-shimmed code paths**: Some low-level components (path virtualization, path truncate helpers, pty primitives) are compiled with `VPROC_SHIM_DISABLED` to avoid recursive interception. These use host syscalls directly and do not consume vproc fd slots.

## Device and path handling
- **Device shims**: vproc handles `/dev/tty`, `/dev/console`, `/dev/ptmx`, `/dev/pts/*`, `/dev/location` (and legacy `/dev/gps` aliases). Opening these paths via shimmed `open` dispatches to vproc-specific handlers (pty creation, location pipe, etc.).
- **Path virtualization**: On iOS/Catalyst, `path_virtualization` truncates absolute paths to the sandbox root (`PATH_TRUNCATE`/`PSCALI_CONTAINER_ROOT`) and special-cases device paths so they bypass truncation and route to vproc devices. Components compiled with `VPROC_SHIM_DISABLED` call host syscalls directly to avoid shim recursion.

## Job control and signals
- vproc tracks per-task pid/pgid/sid and maintains a task table in `gVProcTasks`. Foreground pgid per session is recorded so control characters (^C/^Z) from the session input thread can be mapped to the active job (`vprocDispatchControlSignalToForeground`).
- Signal delivery is cooperative: pending signals are queued and drained on syscall boundaries (`vprocDeliverPendingSignalsForCurrent` in read/write/dup/open, etc.). Default actions mark the vproc as exited/zombie and, when the current thread is the target, cause `pthread_exit`.
- `tcsetpgrp`/`tcgetpgrp` operate on the session pty to mirror Unix terminal semantics for foreground/background jobs.

## PTY lifecycle
- **Creation**: `/dev/ptmx` open creates a new master/slave pair via `pscalPtyOpenMaster`, registers both `pscal_fd` objects in the session pty table, and returns a vproc fd for the master. Slave is exposed as `/dev/pts/N` and inherits permissions from the master or ioctl calls.
- **I/O**: `read`/`write` on vproc fds tied to a pty delegate to the `pscal_fd_ops` in `pscal_tty.c`, which manage line discipline, canonical mode, and winsize. Host threads pumping output (`vprocSessionWriteToMaster`) run outside the vproc shims to avoid deadlock.
- **Winsize**: UI resize events call `PSCALRuntimeUpdateWindowSize`, which updates the session pty winsize and notifies foreground jobs via `SIGWINCH`.

## How it all ties together at runtime
1. Swift runtime launches exsh and installs a shell context (`PSCALRuntimeSetShellContext`).
2. A session pty is created; stdin/stdout/stderr of the initial vproc map to the pty slave.
3. User commands spawn vprocs (per command/job) that inherit the session stdio and job-control metadata.
4. All shimmed syscalls from those processes pass through vproc: fd lookups, device opens, pty ioctls, signal checks, and job-control state updates.
5. Control characters from the UI input thread are detected and dispatched to the foreground pgid; signal delivery is realized when the target vproc hits a syscall boundary.

## When things bypass vproc
- If no current vproc is set on the thread (e.g., code running purely on the Swift side or in a helper thread without `vprocSetShellSelfPid`/`vprocActivate`), libc calls go straight to the host. In that case, there is no virtual pid/pgid, no vpty, and device opens hit the sandbox filesystem directly (e.g., the `/dev/location` stub FIFO).
- Components compiled with `VPROC_SHIM_DISABLED` (path virtualization, truncate helpers, tty primitives) always use host syscalls.

## Practical checkpoints for debugging
- Verify the shims are active: in LLDB, break on `vprocLocationDeviceOpen` or `vprocOpenShim` while `cat /dev/location` runs. If it doesn’t hit, the shim is bypassed.
- Inspect vproc task state via `vprocSnapshot` or the `gVProcTasks` table to ensure pgid/sid/fg_pgid are set when debugging job control.
- For PTY issues, confirm `/dev/ptmx` and `/dev/pts/*` open paths hit `pscalPtyOpenMaster/Slave` and that the session pty entry in `gVProcSessionPtys` is populated.
- For `/dev/location`, a stub FIFO is created (including legacy `/dev/gps` aliases) under the sandbox so `stat`/`ls` succeed even outside a vproc. Each reader blocks until a new payload arrives; partial reads drain a single payload; poll wakes on new data or disable. Reader count changes are observable via `vprocLocationDeviceRegisterReaderObserver`, which the iOS host uses to pause CoreLocation when no readers are present.

This overview should give enough context to reason about how applets, front-end interpreters, nextvi, and other tools interact with the virtual process/pty layers on iOS/Catalyst, and where to look when job control or device handling behaves unexpectedly. 
