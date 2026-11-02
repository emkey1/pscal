# PSCAL V.1R32 Release Notes

Date: 2026-11-02

Base: `V.1R31` (TestFlight build 31, uploaded 2026-11-02)

Status: Not yet uploaded. Not yet run on a physical device.

## Summary
- `V.1R32` is iOS/iPadOS build 32, and it replaces build 31 within a day of that upload. `CURRENT_PROJECT_VERSION` goes 31 -> 32; `MARKETING_VERSION` is untouched.
- Build 31 can abort the whole app the first time it compiles a script or program. This build fixes that, two other iOS descriptor bugs, and the exsh redirect issues listed as known in build 31.
- pscal-core, exsh and SmallCLUE changed. Rea, Pascal and CLike are the same as in build 31. Aether moved only to pin the new pscal-core, and the language stays at `2026-10-30-1`.

## Fixed in the iOS app
- The app no longer aborts with `runtime: kevent on fd 3 failed with 9`. The bytecode cache writer passed a vproc slot number to `fdopen`, which takes a host descriptor. Each cache write put the bytecode into whichever host descriptor had that number, closed it, and left an empty file in the cache. When that descriptor was the Go runtime's kqueue, the app aborted within milliseconds. Any other descriptor was damaged silently. Build 31 has this: an unpatched simulator run died with this error, and the bug dates from 2026-10-20.
- The bytecode cache works on iOS again, because its entries now hold bytecode.
- A regular file no longer looks like a directory. `tail -1 f` could fail with `Is a directory` and `sed s/a/b/ f` with `couldn't edit f: not a regular file`, for about one file in sixteen and for every file near it for the rest of the session. The host `fstat` lookup found `__fstat`, which fills the old struct layout, so `st_mode` held the inode's low bits and `st_size` was 0. The same bug gave `split -l 1 file prefix; cat prefixaa` an empty first piece: with the old layout put back, that failure returned in 2 of 40 test runs. `awk '...' < file` came back empty once in build 31 and has not since, in 260 runs of the later builds, but its cause was not shown.
- The poll wake pipe and the VM's SIGINT pipe stay open. Both are process-wide, but they belonged to the first command that used them and closed when it ended, while their numbers stayed in use. Wake-ups then wrote NUL bytes into whichever files had those numbers next, and draining the pipe could read away another file's data.
- `exsh script > file`, `sh script > file` and `sh -c '...' > file` honor the redirection. A nested shell used to tear down its caller's vproc. It now runs inside its caller and restores `$0`, the positional parameters and the interactive flag when it returns.
- `X=1 env | grep X` finds `X`. An assignment on a command in a pipeline now stays set until the pipeline ends, because the stage's thread reads it after the shell has moved on.
- An unknown command in a pipeline says `exsh: x: command not found`, with status 127, and the script goes on. It used to say `command unavailable in pipeline` and stop the script.
- A command on a worker thread keeps its failure status. `pascal missing.pas; echo $?` printed 0, and `pascal x || y` never ran `y`. The same went for `clike`, `rea`, `pscalvm`, `awk`, `mkdir` and `#!` scripts. The worker dropped its exit as soon as it finished, so the shell's wait found nothing and read a stale status of its own.
- Pascal's `halt(3)` gives status 3. `exit` ends only the program's thread in the app, and the status was lost with it, leaving 127.
- A `#!` frontend program's output reaches its redirection, pipe or `$(...)`. `Examples/clike/base/hello > f` left `f` empty and `x=$(hello)` came back empty, because nothing flushed the program's buffered output. `time hello` had the same problem.
- A redirection to a missing directory fails with `No such file or directory`, as `touch` and `cp` do. `> /nonexistent/dir/f` used to create `nonexistent/dir` in the workspace.
- `find ... -execdir` returns to its directory. It stayed in the first one it visited, so the next path was not found.
- `fopen` on a `/dev/ttyN` device no longer reads a host descriptor that has nothing to do with it. This is the same mistake as the cache writer's: `wc -c /dev/tty3` said `Is a directory`, and closing the stream closed that descriptor.
- `md` and the pagers no longer leak two host descriptors and their unlinked temporary files per run, for as long as the app lives.
- `who`, `users` and `uptime` find the records in a utmp file, and a wtmp record reaches its file. `fcntl`, `pread` and `pwrite` took a slot number for a host descriptor, the cache writer's mistake again: `who file` saw no records, and a record for `/var/log/wtmp` went to whichever host descriptor had that number while the command reported success. `tail -f` and `stty -F` changed `O_NONBLOCK` on an unrelated descriptor the same way: a system directory in one run, the host's fd 0 in others.

## Fixed in exsh on every platform
- `{ ...; } > file`, `if ...; fi > file`, `case ... esac > file` and a redirection on a function definition now cover the whole body, including input redirections, and they close when the body leaves early through `return`, `break` or `exit`. They used to be dropped: the output went to the terminal and the file was never created.
- A redirection on a compound command that cannot be opened now fails that command with status 1 and skips its body, and the script goes on.

## Known Issues
- On iOS, `X=1 cmd | ...` also shows `X` to later stages of the same pipeline and to their word expansion, because all stages share one process environment. `X=1 env | env` differs from bash here. A background `X=1 cmd &` restores `X` at once, so the command may not see it.
- A failed redirection's message says `exec:` even when no `exec` was involved.
- exsh subshells are not separate: `( exit 3 )` ends the script, and a subshell's assignments and `cd` persist. `(echo s)` needs spaces, as in `( echo s )`.
- In `{ a; b; } | cat` only the first command goes through the pipe, `echo o | { read x; }` reads nothing, and `{ ...; } &` runs in the foreground.
- `d=$(false) || ...` ignores the command substitution's status, so `d=$(mktemp -d) || exit 1` carries on with an empty `$d`.
- `echo a &> f` prints a leftover debug line and adds the `;` to the file name. `$-` does not expand.
- On iOS a VM program's `read` and `readln` take the terminal, whatever its stdin is: `pascal prog < file` and `printf '10\n' | pascal prog` wait for the keyboard. A `#!` program does the same.
- `rsync` ends the whole app on any real transfer, with no crash report. `rsync --version` works.
- `time clike prog` says `time: clike: command not found`. `time prog` works for a `#!` program.
- The VM's file builtins (Pascal `Assign`, CLike `fopen`, `mstreamsavetofile`, `fileexists`) do not translate the app's absolute paths. Use a relative path or `$HOME/...`. `/home/x` in a program's own file calls fails.
- `aether --version` reads `1.01-dev` in this build. The language is the frozen 1.0 one; the label comes from `components/aether/RELEASE`, which moved on right after the 1.0 tag.
- The app bundles no Aether guide: `~/Docs` has the PSCAL documents only.

## Validation
Results are recorded here once this commit's build has been tested.
