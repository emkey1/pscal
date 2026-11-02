# PSCAL V.1R32 Release Notes

Date: 2026-11-02

Base: `V.1R31` (TestFlight build 31, uploaded 2026-11-02)

Status: Not yet uploaded. Not yet run on a physical device.

## Summary
- `V.1R32` is iOS/iPadOS build 32, and it replaces build 31 within a day of that upload. `CURRENT_PROJECT_VERSION` goes 31 -> 32; `MARKETING_VERSION` is untouched.
- Build 31 can abort the whole app the first time it compiles a script or program. This build fixes that, two other iOS descriptor bugs, and the exsh redirect issues listed as known in build 31.
- Only pscal-core and exsh changed. Rea, Pascal, CLike and SmallCLUE are the same as in build 31. Aether moved only to pin the new pscal-core, and the language stays at `2026-10-30-1`.

## Fixed in the iOS app
- The app no longer aborts with `runtime: kevent on fd 3 failed with 9`. The bytecode cache writer passed a vproc slot number to `fdopen`, which takes a host descriptor. Each cache write put the bytecode into whichever host descriptor had that number, closed it, and left an empty file in the cache. When that descriptor was the Go runtime's kqueue, the app aborted within milliseconds. Any other descriptor was damaged silently. Build 31 has this: an unpatched simulator run died with this error, and the bug dates from 2026-10-20.
- The bytecode cache works on iOS again, because its entries now hold bytecode.
- A regular file no longer looks like a directory. `tail -1 f` could fail with `Is a directory` and `sed s/a/b/ f` with `couldn't edit f: not a regular file`, for about one file in sixteen and for every file near it for the rest of the session. The host `fstat` lookup found `__fstat`, which fills the old struct layout, so `st_mode` held the inode's low bits and `st_size` was 0.
- The poll wake pipe and the VM's SIGINT pipe stay open. Both are process-wide, but they belonged to the first command that used them and closed when it ended, while their numbers stayed in use. Wake-ups then wrote NUL bytes into whichever files had those numbers next, and draining the pipe could read away another file's data.
- `exsh script > file`, `sh script > file` and `sh -c '...' > file` honor the redirection. A nested shell used to tear down its caller's vproc. It now runs inside its caller and restores `$0`, the positional parameters and the interactive flag when it returns.
- `X=1 env | grep X` finds `X`. An assignment on a command in a pipeline now stays set until the pipeline ends, because the stage's thread reads it after the shell has moved on.
- An unknown command in a pipeline says `exsh: x: command not found`, with status 127, and the script goes on. It used to say `command unavailable in pipeline` and stop the script.

## Fixed in exsh on every platform
- `{ ...; } > file`, `if ...; fi > file`, `case ... esac > file` and a redirection on a function definition now cover the whole body, including input redirections, and they close when the body leaves early through `return`, `break` or `exit`. They used to be dropped: the output went to the terminal and the file was never created.
- A redirection on a compound command that cannot be opened now fails that command with status 1 and skips its body, and the script goes on.

## Known Issues
- On iOS, `X=1 cmd | ...` also shows `X` to later stages of the same pipeline and to their word expansion, because all stages share one process environment. `X=1 env | env` differs from bash here. A background `X=1 cmd &` restores `X` at once, so the command may not see it.
- A failed redirection's message says `exec:` even when no `exec` was involved.
- exsh subshells are not separate: `( exit 3 )` ends the script, and a subshell's assignments and `cd` persist. `(echo s)` needs spaces, as in `( echo s )`.
- In `{ a; b; } | cat` only the first command goes through the pipe, `echo o | { read x; }` reads nothing, and `{ ...; } &` runs in the foreground.
- `d=$(false) || ...` ignores the command substitution's status, so `d=$(mktemp -d) || exit 1` carries on with an empty `$d`.
- A failed redirection on a simple command, such as `echo x > /nonexistent/f`, ends the script; bash reports it and goes on. On iOS a missing directory in that path is created instead.
- `echo a &> f` prints a leftover debug line and adds the `;` to the file name. `$-` does not expand.
- On iOS a front end's failure status is lost: `pascal missing.pas; echo $?` prints 0.
- The VM's file builtins (Pascal `Assign`, CLike `fopen`, `mstreamsavetofile`, `fileexists`) do not translate the app's absolute paths. Use a relative path or `$HOME/...`. `/home/x` in a program's own file calls fails.
- `awk '...' < file` came back empty once in a long test run of build 31, and `split -l 1 file prefix; cat prefixaa` found an empty first piece in 3 of 18. The pipe fix above could explain both, but that has not been shown, so they stay listed.
- `aether --version` reads `1.01-dev` in this build. The language is the frozen 1.0 one; the label comes from `components/aether/RELEASE`, which moved on right after the 1.0 tag.
- The app bundles no Aether guide: `~/Docs` has the PSCAL documents only.

## Validation
Results are recorded here once this commit's build has been tested.
