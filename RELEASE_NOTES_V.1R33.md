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

## Differs from bash
- `kill -INT $$` from inside `$(...)` with an INT trap set ends the substitution too, where bash's would carry on, because exsh's runs in the shell's process.
- A substitution that sets its own trap takes that signal even when it was sent to `$$`, for the same reason.
