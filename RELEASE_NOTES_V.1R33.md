# PSCAL V.1R33 Release Notes

Base: `V.1R32`

## Fixed in exsh
- A signal sent to the shell while `$(...)` runs now waits for it, and the shell's trap runs after the command, as in bash. `x=$(cmd)` assigns, then the trap runs. The trap used to run inside the substitution: its `echo` landed in the variable and its `exit` ended only the substitution, so `trap 'exit 1' TERM; while :; do x=$(echo 1); done` could not be stopped by TERM, and a script went on to the end with status 0.
- `trap - TERM` or `trap '' TERM` inside `$(...)` stays there. It used to change the signal for the whole shell, which could then die of a TERM it had a trap for.
- Ctrl-C during `$(...)` with an INT trap set ends the substitution, and the trap runs afterwards.
- The EXIT trap runs when a signal's trap exits inside `eval` or a sourced file in a loop.
- A trap's `exit` at a loop's own check ends a subshell or function there. The rest of it used to run, and its status replaced the trap's.
- A foreground command dying of a signal no longer runs the shell's trap for that signal, except for Ctrl-C, Ctrl-\ and Ctrl-Z.
- On iOS, a signal with no trap sent to the shell during `$(...)` ends the shell, not only the substitution.
- `tools/shellbench_cacheable/shellbench` gets a number from exsh on every case of `sample/assign.sh` and `sample/eval.sh`.
- `kill SIG 0` signals the shell's own group, on every platform. It said `kill: (0) Invalid argument`.

## Fixed in the iOS app
- `kill -TERM -1` means what it means on Linux: every task but the caller and the app's kernel task, and `No such process` when none is left. It used to mark the kernel task as dying and cancel its thread, and `kill -0 -1` said a task was there when none could be reached.
- A signal to the app's kernel task succeeds and does nothing, as for init on Linux. `kill -TERM 1` could stop the thread that carries Ctrl-C.
- A signal to a group stays in the app, from any thread. From a thread with no task of its own, `0`, `-1` and `-N` went to iOS's `kill`, which reads them as real process groups, and `-1` as every process the app may signal. Only a positive pid can reach iOS's `kill` now.
- `kill(0, sig)` from a program acts as `kill(-pgid, sig)` does. SIGINT to your own group is queued for the next check in both forms. `kill(0, SIGINT)` used to mark the task dead while it ran on.

## For iOS-flavoured builds on a host
- The real root is never provisioned. `PATH_TRUNCATE=/`, any spelling or symlink that comes to it, and `HOME=/` with nothing else set used to make directories in the root, prune `/proc` there and rename links over `/usr/bin` names. `$HOME` stands in now, as path translation already did, and nothing is provisioned when `$HOME` is the root or unset.

## Differs from bash
- `kill -INT $$` from inside `$(...)` with an INT trap set ends the substitution too, where bash's would carry on, because exsh's runs in the shell's process.
- A substitution that sets its own trap takes that signal even when it was sent to `$$`, for the same reason.
- Every child gets a process group of its own, with job control off too, where bash leaves a script's children in the shell's group. So `kill 0` in a script reaches only the shell, and `kill -- -$!` finds the job where bash says no such process.

## Differs from Linux on iOS
- A fatal signal a subshell sends itself returns: `( kill -TERM $BASHPID; echo x )` prints `x`, though its status is 143.
- A signal to a task that has ended but not been waited for says `No such process`, where Linux says 0.
- SIGINT or SIGTSTP to your own group on your own thread is queued for the next check, for programs that check, where Linux acts before `kill` returns.
- The task table has no user id, so anyone can signal anyone. That is Linux for one user, and `EPERM` belongs here once the app has users.
