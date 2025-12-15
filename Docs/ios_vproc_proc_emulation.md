# iOS vproc process emulation gaps vs. iSH (clean-room plan)

Goal: bring PSCAL’s iOS/iPadOS synthetic process model up to parity with iSH’s task/process lifecycle (minus CPU emulation), so job control, wait/kill/fg/bg/pgid/sid behave reliably in the shell and frontends.

## What iSH does
- Global PID table (`pids_lock`) with parent/child links, zombie tracking, exit codes, pgid/sid, group exit vs. per-thread exit, and ptrace/signal state.
- Thread-per-task with per-task comm name, exit/zombie lifecycle, and wait semantics that distinguish group exit vs. per-thread exit.
- pgid/sid management integrated with session/tty state; wait/kill can target groups.

## Current PSCAL vproc
- Synthetic PID table with pid/tid/status/stopped/exited/job_id/label only; no parent/child/pgid/sid/zombie.
- Signals delivered per-pid; no blocked/pending masks or pgid/sid targeting.
- Wait clears the entry on exit; stopped tasks persist.
- No comm/truncated command field or thread-name updates.
- Shell job control relies on vprocSnapshot + job table, but lacks true process group/session semantics.

## Plan (clean-room)
1) **PID table & lifecycles**
   - Add parent_pid, pgid, sid, exit_code, zombie flag, and comm (truncated) to VProcTaskEntry.
   - Maintain parent/child links in vprocTaskTable; preserve zombies until reaped (wait).
   - Update vprocWaitPidShim to support wait on pgid/sid (-pgid), return zombies, and clear only when reaped.
   - Set thread name to `comm-pid` on create for debugging.
2) **Process groups/session**
   - Add vprocSetPgid/vprocSetSid APIs; default pgid=sid=pid on create.
   - Allow vprocKillShim to target negative pid (pgid) and session (sid optional), mirroring iSH group delivery.
   - Track a “shell sid” to avoid killing the UI shell.
3) **Signal/state handling**
   - Track last_signal, stop_signal, exit_signal in entry; simulate WIFSTOPPED/WIFSIGNALED per iSH rules.
   - Keep stopped tasks in table; mark zombies on exit; wake waiters appropriately.
4) **Shell integration**
   - When registering jobs, also set pgid/sid on vproc entries; fg/bg/wait/kill should honor pgid and zombies.
   - Ensure `%N` and `jobs` enumerate vproc zombies/stopped and avoid renumbering.
5) **Tests**
   - Extend `Tests/ios_vproc` to cover: pgid/sid set/get, kill -pgid delivery, zombie reaping vs. running count, comm/truncated labels, thread-name set.
   - Flesh out shell-level `%N` regression (jobspec_parse.sh) once tool dispatch passes script paths.

Notes:
- No CPU emulation: lifecycle/state only.
- Keep clean-room (no GPL import); model behaviour, not code.
