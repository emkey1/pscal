# Shell Task Registry & Synthetic PID Design

## 1. Overview
exsh currently executes every builtin and frontend inside a single VM, so long-running tools (nextvi, Pascal, etc.) block the shell until they exit. Threads/GUIs show up as raw VM threads when you run `ps`, which is confusing on iPad because the floating nextvi window looks and behaves like a separate process. This doc fleshes out the task registry model discussed earlier so we can:

- expose long-lived tools as independent “processes” with synthetic PIDs/PGIDs,
- let the shell stay responsive while they run,
- give `ps`, `jobs`, `fg/bg`, and `kill` a task-oriented view instead of raw threads.

## 2. Goals & Scope
1. **Task registry API** that tracks synthetic PIDs, state, command lines, and controller threads.
2. **Tool runner integration** so any frontend (nextvi, Gwin, Pascal, etc.) launched via `shellRunTool*` automatically becomes a task and does not block exsh.
3. **Command updates** for `ps`, `jobs`, `fg/bg`, and `kill` so they operate on tasks.
4. **Signal routing** so `kill <pid>` reaches the correct thread/GUI helper.
5. **Documentation/tests** covering the new behavior on both desktop and iPad.
6. **While this is intended primarily for the iOS/iPadOS port, it should be optionally available at runtime on the standalone version of exsh via setting 'FULL_TASK_SUPPORT' to 1.

Out of scope for v1: multi-task pipelines, suspending/resuming arbitrary tasks, cross-session persistence.

## 3. Architecture
### 3.1 Data Structures
```
typedef enum {
    TASK_STATE_RUNNING,
    TASK_STATE_STOPPED,
    TASK_STATE_EXITED
} TaskState;

typedef bool (*TaskSignalHandler)(struct TaskRecord *task, int signo);

typedef struct TaskRecord {
    int pid;
    int pgid;
    char *command;
    FrontendKind kind;
    TaskState state;
    int exitStatus;
    time_t startTime;
    pthread_t controllerThread;
    TaskSignalHandler handler;
} TaskRecord;
```

### 3.2 Registry API
Location: `src/shell/task_registry.{c,h}`
- `void taskRegistryInit(void);`
- `int taskRegistryCreate(const char *cmdline, FrontendKind kind, pthread_t controller, TaskSignalHandler handler);`
- `void taskRegistryUpdateState(int pid, TaskState state);`
- `void taskRegistrySetExitStatus(int pid, int status);`
- `TaskRecord *taskRegistryLookup(int pid);`
- `void taskRegistryList(void (*cb)(const TaskRecord *, void *ctx), void *ctx);`
- `void taskRegistryDestroy(int pid);`
All operations protected by a mutex. PID counter wraps at INT_MAX; on collision we recycle oldest exited task.

### 3.3 Tool Runner Integration
- Update `shellRunToolBuiltin` and `shellRunToolInThread` to create/destroy tasks automatically. Save the synthetic PID in TLS so nested helpers (error reporting, logging) can print `[task 123]`.
- `smallclueRunnextvi` becomes a lightweight wrapper that launches the GUI via the tool runner rather than inline. Same for Gwin and future GUI helpers.
- CLI frontends (pascal, clike, etc.) launched via `shellRunToolBuiltin` already go through the runner; they just need the new task registry hooks.

### 3.4 Command Updates
- **ps**: default view enumerates task registry. Add `--threads` to show the legacy VM thread list for debugging.
- **jobs**: show `[n] pid state command`. `%n` maps to nth entry in the registry.
- **fg/bg**: `fg PID` or `fg %n` sets that task as foreground. For GUI tasks we focus the window via `editorWindowManager`. `bg PID` just clears the “needs attention” flag.
- **kill**: resolves PID via registry and calls the stored `TaskSignalHandler`. GUI handlers map SIGINT/SIGTERM to `editor_exit`/`gwin_exit`; other tasks get `pthread_kill`.
- `$!` exports last-created task PID. `$?` still reflects exit status per POSIX.

### 3.5 Signal/State Handling
- Tasks mark themselves STOPPED when they relinquish the TTY (e.g., nextvi floating window loses focus) and RUNNING when they regain it. For v1 we only expose RUNNING/EXITED.
- Ctrl+C dispatch: shell keeps `currentForegroundPid`; if set, `Ctrl+C` sends SIGINT to that task instead of self.

## 4. Implementation Plan
### Milestone 0 – Prep (1 day)
- Create `Docs/task_registry_design.md` (this file) and link from `Docs/ios_build.md`.
- Add build stubs for new module.

### Milestone 1 – Registry + Hooks (2 days)
- Implement `task_registry.{c,h}` with unit tests (native build).
- Add TLS helpers (`int shellCurrentTaskPid(void)`) for logging.
- Wire registry calls into `shellRunToolBuiltin`, `shellRunToolInThread`, `shellRunToolProcess`.

### Milestone 2 – GUI frontends async (3 days)
- Convert `smallclueRunnextvi`/`smallclueRunGwin` to use the tool runner path so the shell isn’t blocked.
- Provide `TaskSignalHandler` implementations for nextvi/Gwin.
- Ensure cleanup occurs even when `editor_exit` longjmps.

### Milestone 3 – Command integration (3 days)
- Rewrite `ps`, `jobs`, `kill`, `fg`, `bg` to use the registry.
- Update shell prompt logic for `$!`.
- Extend `ps` to expose `--threads`.

### Milestone 4 – Testing & polish (2 days)
- Add automated tests (shell scripts) verifying `ps/jobs/kill` behavior.
- Manual iPad pass: run nextvi & other frontends, confirm they show up with synthetic PIDs and the main shell stays responsive.
- Documentation updates (README/Docs/ios_build.md). Note behavior change in release notes.

Total rough estimate: ~8–9 engineering days plus review cycles.

## 5. Risks & Mitigations
- **Global state races**: ensure task creation happens before the frontend touches shared globals; use existing `pascalPushGlobalState` hooks to guard.
- **Nested tool invocations**: if a task launches another tool (e.g., nextvi spawns Gwin), decide whether that becomes a child task (new PID) or runs inline. Proposal: child task with its own PID but same PGID.
- **Backward compatibility**: gate the task view behind a compile-time flag (`PSCAL_ENABLE_TASK_REGISTRY`) until thoroughly vetted. On legacy builds, fall back to current behavior.
- **Resource leaks**: add watchdog in registry to reap exited tasks on timeout in case a tool crashes without calling destroy.

## 6. Deliverables
- `src/shell/task_registry.{c,h}` + tests.
- Updates to `shellRunTool*`, smallclue GUI launchers, and shell builtins.
- Docs (this file + release notes) explaining synthetic PIDs and how to interact with them.
