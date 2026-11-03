# PSCAL V.1R32 Release Notes

Date: 2026-11-02

Base: `V.1R31` (TestFlight build 31, uploaded 2026-11-02)

Status: Not yet uploaded. Not yet run on a physical device.

## Summary
- `V.1R32` is iOS/iPadOS build 32, and it replaces build 31 within a day of that upload. `CURRENT_PROJECT_VERSION` goes 31 -> 32; `MARKETING_VERSION` is untouched.
- Build 31 can abort the whole app the first time it compiles a script or program. This build fixes that, two other iOS descriptor bugs, and the exsh redirect issues listed as known in build 31.
- pscal-core, exsh, SmallCLUE and openrsync changed. Rea, Pascal and CLike are the same as in build 31. Aether moved only to pin the new pscal-core, and the language stays at `2026-10-30-1`.

## Fixed in the iOS app
- The app no longer aborts with `runtime: kevent on fd 3 failed with 9`. The bytecode cache writer passed a vproc slot number to `fdopen`, which takes a host descriptor. Each cache write put the bytecode into whichever host descriptor had that number, closed it, and left an empty file in the cache. When that descriptor was the Go runtime's kqueue, the app aborted within milliseconds. Any other descriptor was damaged silently, including a redirect's own file: where `> out` got the writer's number, `tiny a.tiny a.pbc > out` left all of `tiny.clike`'s compiled entry, about 21 KB, in `out`. Build 31 has this: an unpatched simulator run died with this error, and the bug dates from 2026-10-20.
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
- Two frontends can run at once. `clike a | clike b`, or two background `clike` jobs, crashed the whole app, and `clike a | pascal b` could fail with a spurious `Undefined variable`. Compile state was shared by the whole process, and it now belongs to each command's thread, as do a program's `TRUE`/`true` style, its write spacing, its Aether string base, its colours and its `--deny`.
- An interface's method table belongs to one program run. `pascal a.pas; pascal b.pas` with the same class and interface names failed with `Interface method slot 1 out of range`, or called the first program's code, for the rest of the session.
- A CLike `printf` is one write, so background jobs sharing a terminal no longer interleave it mid-line. Each character used to be its own system call.
- A wakeup no longer writes a NUL into a command's stderr. Replacing stderr during a command, as Aether and Pascal both do, left a saved descriptor number that was closed a second time when the command ended, and that could close the poll wake pipe.
- `md` and the pagers no longer leak two host descriptors and their unlinked temporary files per run, for as long as the app lives.
- `who`, `users` and `uptime` find the records in a utmp file, and a wtmp record reaches its file. `fcntl`, `pread` and `pwrite` took a slot number for a host descriptor, the cache writer's mistake again: `who file` saw no records, and a record for `/var/log/wtmp` went to whichever host descriptor had that number while the command reported success. `tail -f` and `stty -F` changed `O_NONBLOCK` on an unrelated descriptor the same way: a system directory in one run, the host's fd 0 in others.
- `rsync` no longer ends the whole app, and local transfers work. The app had no way to start a local transfer's server side, and the `_exit` after the failed `exec` was a real one, so any transfer ended every terminal session with no crash report. The server now runs as a thread, with its own options, rules and paths, and a transfer that cannot run fails with a message and a status.
- `scp` and `sftp` no longer take the app down when their transport fails, and they end with status 255. `scp -S nope file host:x` ended every terminal session with no crash report: scp's SIGPIPE handler ran on the main thread, and its `_exit` was a real one. Both also closed their one socket twice, so the second close could hit a host descriptor that had the same number by then. In one traced run of `scp file nosuch.invalid:x` that was the Go runtime's kqueue, and the app aborted with `runtime: kevent on fd 8 failed`. Ten untraced runs of the old app did not abort.
- `ls /proc/self/fd`, `ls /proc/$$/fd` and `/proc/<pid>/fdinfo` show that command's own descriptors. They listed the app's host descriptors, whichever command asked. The last raw `fcntl` calls that took a slot number for a host descriptor are gone as well: `ReadKey` gives up on a lone ESC from a pipe after 100 ms instead of waiting until the writer ends, `vi`'s full sync after a write reaches the file it wrote, and the shell no longer sets close-on-exec on the Go runtime's pipes.

## Fixed in exsh on every platform
- `{ ...; } > file`, `if ...; fi > file`, `case ... esac > file` and a redirection on a function definition now cover the whole body, including input redirections, and they close when the body leaves early through `return`, `break` or `exit`. They used to be dropped: the output went to the terminal and the file was never created.
- A redirection on a compound command that cannot be opened now fails that command with status 1 and skips its body, and the script goes on.
- What exsh starts sees only exported variables, as in bash: `x=1; env` no longer lists `x`, and a big variable no longer makes later commands fail with `Argument list too long`. On iOS the same goes for applets, frontends and scripts, `X=1 cmd | ...` shows `X` only to `cmd`, and a background `X=1 cmd &` sees it. What a program sets stays with that program.
- `y=$(echo "$v")`, `( ... )`, `{ ...; } | cat` and `&` inside a function see the function's locals. A substitution came back empty on every platform.

## Known Issues
- A failed redirection's message says `exec:` even when no `exec` was involved.
- exsh subshells are not separate: `( exit 3 )` ends the script, and a subshell's assignments and `cd` persist. `(echo s)` needs spaces, as in `( echo s )`.
- In `{ a; b; } | cat` only the first command goes through the pipe, `echo o | { read x; }` reads nothing, and `{ ...; } &` runs in the foreground.
- `d=$(false) || ...` ignores the command substitution's status, so `d=$(mktemp -d) || exit 1` carries on with an empty `$d`.
- `echo a &> f` prints a leftover debug line and adds the `;` to the file name. `$-` does not expand.
- On iOS a VM program's `read` and `readln` take the terminal, whatever its stdin is: `pascal prog < file` and `printf '10\n' | pascal prog` wait for the keyboard. A `#!` program does the same.
- On iOS a background command that reads a redirected stdin stops as if it were reading the terminal: `cat < f > out &` never finishes, and a `wait` for it never returns. `( cat < f > out ) &` works.
- `ssh` in the app does not connect a remote command to the local stdin, stdout or a redirection: `ssh host cmd > f` leaves `f` empty, and `echo x | ssh host cat` sends nothing. So `rsync` to or from a remote host fails with `unexpected end of file`, and the app stays up. Local transfers are not affected.
- On iOS a redirection does not catch `scp` or `sftp` messages: when the transport fails, `scp file nosuch.invalid:x > f 2>&1` leaves `f` empty, and only the status of 255 tells you.
- `time clike prog` says `time: clike: command not found`. `time prog` works for a `#!` program.
- The VM's file builtins (Pascal `Assign`, CLike `fopen`, `mstreamsavetofile`, `fileexists`) do not translate the app's absolute paths. Use a relative path or `$HOME/...`. `/home/x` in a program's own file calls fails.
- `aether --version` reads `1.01-dev` in this build. The language is the frozen 1.0 one; the label comes from `components/aether/RELEASE`, which moved on right after the 1.0 tag.
- The app bundles no Aether guide: `~/Docs` has the PSCAL documents only.

## Validation
Results are recorded here once this commit's build has been tested.
