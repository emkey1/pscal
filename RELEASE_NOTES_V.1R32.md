# PSCAL V.1R32 Release Notes

Date: 2026-11-05

Base: `V.1R31` (TestFlight build 31, uploaded 2026-11-02)

Status: Not yet uploaded. Not yet run on a physical device.

## Summary
- `V.1R32` is iOS/iPadOS build 32. `CURRENT_PROJECT_VERSION` goes 31 -> 32; `MARKETING_VERSION` is untouched.
- Build 31 can abort the whole app the first time it compiles a script or program. This build fixes that and every other way of ending the app that testing turned up: killing a job as it starts a pipeline, starting `ssh`, `rsync`, `scp`, `nohup`, `lps`, two commands parsing their options at once, `printf '%n'`, and a graphics window that closes as it opens.
- exsh on iOS now behaves far more like bash: redirections on compound commands, real subshells, `wait`, traps, `$(...)`, jobs and what they can see.
- pscal-core, exsh, SmallCLUE and openrsync changed most. Rea, Pascal and CLike changed for what a run leaves behind. Aether's own source changed only for per-thread compile state and what a parse leaves behind, and the language stays at `2026-10-30-1`.

## What changes for testers
- `set -e` at the prompt can now close the tab when a line fails with another command after it.
- Options set in `~/.exshrc` now reach the prompt.
- A prompt line leaves its state for the next: traps, `readonly`, `$!`, `pushd`, `hash`, `enable -n`, arrays and `getopts`. `$!`, `${!name}` and `[!...]` are no longer taken for history references. An EXIT trap runs at Ctrl-D and on `exit`, once. A nested shell starts without the caller's traps and jobs.
- A line too long for the terminal's buffer loses the excess, as on Linux: pasting a long line into `cat` now loses what does not fit. It used to hang the keyboard.

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
- A frontend run leaves almost nothing behind. Each Pascal, CLike, Rea or Aether run leaked a routine's local table and the parser's tokens for the rest of the session, about 118 KB for a 60-routine program, and now about 1.5 KB.
- A background frontend prints nothing of its own. `clike x &` wrote four `[tool-inline]` lines to stderr.
- In a group or subshell, each background job has its own job number and a prompt does not announce them. A prompt's `[1] 123` goes to stderr, not stdout, as under bash.
- A wakeup no longer writes a NUL into a command's stderr. Replacing stderr during a command, as Aether and Pascal both do, left a saved descriptor number that was closed a second time when the command ended, and that could close the poll wake pipe.
- `md` and the pagers no longer leak two host descriptors and their unlinked temporary files per run, for as long as the app lives.
- Redirecting stdin inside a group or loop whose own stdin is already redirected no longer leaks. `{ ...; cat < f; ...; } < g` kept two host descriptors for each `cat < f`, for as long as the app lived: 600 rounds left 1,200 open.
- A program's own file calls follow `cd` and take the app's absolute paths. After `cd d`, `pascal p` with `assign(f, 'x'); rewrite(f)` wrote `x` in the directory the app started in, with no error, while `pwd` said `d`, and a read took that directory's copy. This covered Pascal, CLike, Rea and Aether: reads, writes, appends, renames and erases, `fileexists`, `mstreamloadfromfile`, `mstreamsavetofile`, `toon_parse_file`, sqlite databases and `file://` requests, from threads too. `/home/x` in those calls failed. `getcurrentdir` said the start directory and now agrees with `pwd`.
- `pascal`, `clike` and `rea` find their source after a `cd` inside `sh -c`, an `exsh` script or a `#!` script. They ran in the directory of the shell that started the script, so `sh -c 'cd d; pascal p'` said `Error opening source file`, or ran that directory's `p`.
- `who`, `users` and `uptime` find the records in a utmp file, and a wtmp record reaches its file. `fcntl`, `pread` and `pwrite` took a slot number for a host descriptor, the cache writer's mistake again: `who file` saw no records, and a record for `/var/log/wtmp` went to whichever host descriptor had that number while the command reported success. `tail -f` and `stty -F` changed `O_NONBLOCK` on an unrelated descriptor the same way: a system directory in one run, the host's fd 0 in others.
- `rsync` no longer ends the whole app, and local transfers work. The app had no way to start a local transfer's server side, and the `_exit` after the failed `exec` was a real one, so any transfer ended every terminal session with no crash report. The server now runs as a thread, with its own options, rules and paths, and a transfer that cannot run fails with a message and a status.
- `scp` and `sftp` no longer take the app down when their transport fails: they print what went wrong and end with status 255, or 1 with `scp -O`. `scp -S nope file host:x` ended every terminal session with no crash report: scp's SIGPIPE handler ran on the main thread, and its `_exit` was a real one. Both also closed their one socket twice, so the second close could hit a host descriptor that had the same number by then. In one traced run of `scp file nosuch.invalid:x` that was the Go runtime's kqueue, and the app aborted with `runtime: kevent on fd 8 failed`. Ten untraced runs of the old app did not abort.
- `ssh` connects a remote command to the local stdin, stdout and redirections. `ssh host cmd` opened a tab of its own and reported 0 at once, so `ssh host cmd > f` left `f` empty, `ssh host cmd | cat` printed nothing and `echo x | ssh host cat` sent nothing. It now runs where it was started and returns the remote command's status, binary data comes through both ways, and ^C stops it with status 130. `ssh host` at a terminal still opens a login tab.
- `rsync`, `scp` and `sftp` work with a remote host, both ways. The `ssh` they start closed every descriptor from 3 up by number, including host descriptors behind its own stdin and stdout, so `rsync` failed with `unexpected end of file`. `rsync -a`, `scp`, `scp -O` and `sftp -b` were tested against a real `sshd`, as was `sftp-server` killed mid-transfer, which ends with a message and status 255 with the app up.
- `rsync` pulling from a host that runs the `rsync` in macOS ends with status 0. The client closed its connection right after the transfer, and that server exits 14 if it sees the end of its input before it ends, with everything already transferred. The client now gives a server up to 2 seconds to end first.
- `rsync` no longer reports an error after a complete transfer. If the sender stalled for a moment at its last step, the other side's good-bye was taken for a request or its departure for a failure: `phases complete with files still queued` or `poll: hangup`, with every file in place. The app's smoke test hit this once under load.
- `scp` takes a name like `ui/x` as a file. A name of two letters ending in `i`, `F` or `S` before the slash was read as an option, so `scp ui/x host:y` said `stat local "ui/Users/...": No such file or directory`.
- `ls /proc/self/fd`, `ls /proc/$$/fd` and `/proc/<pid>/fdinfo` show that command's own descriptors. They listed the app's host descriptors, whichever command asked. The last raw `fcntl` calls that took a slot number for a host descriptor are gone as well: `ReadKey` gives up on a lone ESC from a pipe after 100 ms instead of waiting until the writer ends, `vi`'s full sync after a write reaches the file it wrote, and the shell no longer sets close-on-exec on the Go runtime's pipes.
- `socketpoll` and `socketclose` act on the socket they're given. With a shell descriptor open at the same number as a socket, `socketpoll` reported readiness that wasn't there, and `socketclose` closed that descriptor and left the socket open.
- A command with stdout opened for reading no longer leaks host descriptors. `echo x 1<file` leaked about four per run and `pascal p 1<file` about five, for as long as the app lived.
- A history file past `HISTFILESIZE` keeps its last lines. Trimming it replaced the file with an empty one. This was measured on the host build of the iOS shell, not in the app.
- `wait` gives a killed background job's status. `( sleep 30 ) & p=$!; kill $p; wait $p` could give 0, and now gives 143 every time. Killing a job also harms nothing else. A job killed in `wait` used to hang the shell, a job killed in `$(...)` left the app's stdout on its pipe, a loop that made no calls ran on after its status was reported, and what the job had started stayed in `lps` holding its pipes, so a reader of them never finished.
- A background job's `$(...)` takes stdout from that job alone. It used to move the app's own, so beside a job the shell's output could vanish or land in the job's variable, and the shell could end. A job's `$(echo hi)` could also come back empty with `hi` on the screen. `sh x | cat` now returns when `x` killed a job inside `$(...)`; it used to hang with its output complete.
- `lps` could crash the app. Finding the CPU time of a task whose thread had ended used the name that thread's Mach port used to have. By then the name could belong to something else, so the app could die at once or release a right it still needed.
- A job that ended while a `$(...)` ran no longer hangs `wait`: `true & x=$(echo e); wait` almost never returned. `x=$(sleep 1 & echo bg)` at a prompt no longer captures the `[1] pid` line.
- `timeout` works. `timeout 5 echo hi` said `timeout: waitpid: No child processes`, status 125, because the applet forked and the app has nothing to fork. It now runs an applet, a frontend or a script as a child task with the caller's descriptors and environment. A limit that expires gives 124, otherwise the command's own status comes back, `timeout 0` means no limit, and a missing command gives 127. The command can read the terminal and Ctrl-C reaches it, and the signal reaches what a nested `sh -c` runs.
- A prompt stays a prompt. `sh -c ...` told the whole app it was not interactive until it returned, so a background one turned job control off at the prompt while it ran, and one that was killed turned it off for good, after which `awk '{print}' < f` printed nothing. The setting is now each shell's own.
- A background job can read a file. `cat < f &` stopped and never went on, and `awk '{print}' < f &` printed nothing, because a read of any redirected stdin counted as a background job reading the terminal. Only the terminal stops a reader now.
- `cat f | tail -f` ends, and `tail -f f | head -1` ends with `head`. `fstat` called every in-process pipe a character device, so `tail` did not see a pipe on either side. `stat -` says FIFO on one now.
- `who -m file` finds the terminal's line. `ttyname` took a slot number for a host descriptor, the cache writer's mistake again. `who am i` at a prompt still prints nothing, see Known Issues.
- `halt -w`, `poweroff -w` and `reboot -w` write their record and return at the prompt. They reached the VM's `Halt` routine, which said `Halt expects 0 or 1 integer argument.` and stopped a sourced script there.
- Killing a job while it was starting a pipeline no longer ends the whole app, which it did about 3 times in 100 on that form. The job's thread was still using its parent's process entry when the kill freed it.
- A killed command blocked opening, reading or writing a FIFO or socket now ends, and `wait` reports it only once it has, with what it held closed. It no longer takes the next writer's data.
- Starting `ssh` no longer risks crashing the app. It replaced the app's whole environment as it started, so anything setting a variable at that moment could corrupt memory. This applied to `rsync`, `scp` and `sftp` too, since they start `ssh`.
- Two `ssh`, `scp` or `sftp` commands at once keep their own arguments. They shared their option parsing and, for `scp` and `sftp`, all their state, so two started together could take each other's host or lose an option such as a port. In a test of two at once, 14 of 60 `ssh -G`, 3 of 40 `scp` and 1 of 40 `sftp` gave a wrong result; none do now.
- `scp` ends with status 0 after a good copy, so `scp a host:b && next` reaches `next`. `scp -3`, `scp -3 -O` and `scp host:a host:b .` work too.
- A `Match exec` line in `ssh_config` no longer ends the whole app. `ssh` says `Unable to execute` and ends with status 255.
- Two `rsync --delete` at once no longer end the app. Its lookup table was one per process, so the second destroyed the first's while it was in use.
- Commands beside each other keep their own option parsing. `tee` could lose its `-a` and truncate its file, write a file named `-a`, or read past its arguments and end the app.
- `dirname` and `basename` beside each other keep their own answers. `dirname /aaa/one` could print what another `dirname` asked for, with status 0 and no message, and `scp` sends `basename`'s answer as the file's name.
- Loops that start together keep their own description. An `until` in a job or pipeline stage could lose its condition and never end, and a `for` could run without its redirections and take their words as values. `which`, `env`, `tput` and `mount` used the same call, whose place in a string was one variable for the whole app.
- `nohup` runs what it is given. A name the host's `/bin` also has, such as `sh`, `ls` or `sleep`, replaced the whole app with no crash report, and anything else failed with `No such file or directory`. Now it runs an applet, a frontend or a script, with hangups ignored.
- `wait` no longer hangs on a job nobody started. A scan of the task table could read past its buffer when a task started at that moment, and enter what lay there as a job no thread would ever finish. In a test of four background pipelines per turn: 12 hangs in 72,000 turns before, none in 231,000 after.
- A trap runs in the app, typed at the prompt or in a file. `trap` put its handler on a throwaway process entry for that one builtin, so a trapped signal was lost, and one sent by a job cancelled the shell's thread: a tab with no shell in it and no error. A trapped signal now interrupts `wait` and runs at the next command, and `kill -CONT` continues a stopped job whatever its disposition.
- `printf '%n' x` no longer ends the whole app. `printf` handed any conversion it had no case for to libc, with a string for its argument, and Darwin's libc aborts on `%n`.
- A second graphics program in one session could show a black screen; it now draws.
- Pascal programs that use the 3D builtins, such as Checkers3D, could end at once or take the app down. `GLBegin` now accepts `quad_strip` and `polygon`, and a graphics window that closes straight after opening no longer crashes the app.
- A redirection stays where it was put across `$(...)`. `( x=$(echo a); echo "$x" ) > f`, the same with `if` and `for`, `exec > f`, and a `#!` script run with `> f` left `f` short and wrote the rest to the real stdout, from the first substitution on.
- Every redirection on a background command acts, in order, and a stdio slot that was closed stays closed: a write gives `Bad file descriptor` rather than reaching some other descriptor with that number.
- `exec` and `env` run an applet, a frontend or a script as a call, and a stale inherited `$PWD` is checked.
- App Diagnostics reads the shell's transcript once it has settled, and checks `hostname` by its help text.
- A VM program reads the file or pipe it is given. `pascal prog < file` and `printf '10\n' | pascal prog` waited for the keyboard, in every language, and a `#!` program did the same. Typing at a program still works.
- A program waiting in `eof` no longer stops the others. In `while (!eof(stdin))` it held a lock every VM program needs to start, so no other program started and lines typed at that prompt did not run until its input came. With `tail -f f | clike p &` that was for good.
- Killing a background pipeline ends the VM program in it. `sleep 300 | clike p > /dev/null & kill $!` left the program running, and it took the next line typed. On an iOS-flavoured host build the same form crashed the process every time; no crash was seen in the app.
- A nested shell after a `#!` program in a pipe no longer prints `Internal error: globalSymbols hash table is NULL.`
- A pipeline with a DEBUG trap set no longer ends the app. A trap on the shell's thread cleared the pipeline's state while a stage's thread was reading it. A trap now runs in a copy of the shell's state while a pipeline runs, whichever trap it is.
- A failed expansion in a later stage of a pipeline no longer ends the app. `set -u; f() { sleep 1 | cat $nope; }; f` ended the shell with the first stage still starting, most of the time in a function, `eval` or a sourced file.
- A kill can no longer stop every frontend that comes after it. A kill landing as a frontend finished could leave a lock held, and every later `pascal`, `clike`, `rea` or `aether`, in any tab, then hung as it ended until the app restarted.
- `ssh`, `scp` and `sftp` starting together no longer risk ending the app when a locale is set, for example after `export LANG=en_US.UTF-8`. `setlocale` changed the whole app's locale and aborted if two threads loaded one at once. A command's locale is now its own, `wc -m` included.
- `ssh -o IdentityAgent=...` no longer risks ending the app. It changed the whole app's environment while the shell was reading it for a command. A client now changes only its own environment.
- `ssh -n`, and `ssh host cmd` with `/dev/null` as stdin, stdout or stderr, work. `poll` here doesn't support devices, so `ssh` ended with status 255 and `channel 0: invalid rfd` the first time it touched one.
- `ssh -f` no longer risks ending the app. It called a `daemon()` that forks for real and ends the parent. It now fails with a message and status 255; backgrounding isn't supported.
- A background job's `export` or `unset` no longer crashes the app. It changed the environment while the shell read it to start its next command.
- `f & unset -f f` in a loop no longer hangs the shell.
- A background command or pipeline stage behaves like a forked child: what it changes stays in it. `f() { v=2; }; f & wait; echo "[$v]"` gave `[2]`, `read v <<< x &`, `cd d &`, `cd d | cat` and `echo x | read v` all reached the shell, and `true | exit 3` ended it. Each now keeps its own variables, directory, parameters, definitions and options, and has the call stack it was started from, so `caller`, `FUNCNAME` and `BASH_SOURCE` show what called it.
- Every task has its own file mask, kept beside its directory. A job's `umask` no longer applies to everything else while the job runs, what it creates gets its own mask, and `umask 077 | cat` leaves the shell's alone.
- `rsync` no longer leaves the whole app's umask at 0, which made every later file `-rw-rw-rw-`.
- A DEBUG trap's `exit` now stops the command it comes before, and no longer risks ending the app when a pipeline's stages are running.
- A function that recurses through `$(...)` no longer ends the app. Five or more levels, as in `sum() { ...; echo $(( $1 + $(sum $(( $1 - 1 ))) )); }; sum 5`, wrote to memory the shell had already moved.
- One tab's `$(...)` no longer disturbs other tabs. It unset and restored every variable in the environment tabs share, so another tab could briefly find no `HOME` or use a path untranslated. `$(...)` now changes only what differs, and listing variables with `export -p`, `set` or `${!x*}` reads a safe copy.
- Killing the last stage of a pipeline from another task, such as `kill` in another tab or a job, could cancel the tab's shell and leave a tab with no prompt. Only the stage ends now. A foreground subshell or command killed from another task also stops there, where it used to run to its end and only report 143.
- Closing a tab whose shell won't end no longer takes another tab's shell. The first tab used to stay on screen with nothing reading keys and its jobs gone, while the closed tab kept running.
- Replacing `/etc/passwd` or `/etc/group` while another command looks up a name, such as `whoami`, `id`, `ls -l`, `stat`, `find` or `tar`, can no longer end the app. The tables had no lock, and a reload freed one under its readers.
- `kill -TERM -1` means what it means on Linux: every task but the caller and the app's kernel task, and `No such process` when none is left. It used to mark the kernel task as dying and cancel its thread, and `kill -0 -1` said a task was there when none could be reached.
- A signal to the app's kernel task succeeds and does nothing, as for init on Linux. `kill -TERM 1` could stop the thread that carries Ctrl-C.
- A signal to a group stays in the app, from any thread. From a thread with no task of its own, `0`, `-1` and `-N` went to iOS's `kill`, which reads them as real process groups, and `-1` as every process the app may signal. Only a positive pid can reach iOS's `kill` now.
- `kill(0, sig)` from a program acts as `kill(-pgid, sig)` does. SIGINT to your own group is queued for the next check in both forms. `kill(0, SIGINT)` used to mark the task dead while it ran on.
- `halt`, `reboot` and `poweroff` without `-w` end the tab, as `exit` does, from the prompt, a script, a pipeline or a subshell. In the first tab they ask what `exit` asks, `This will exit the app. Are you sure?`. They used to print `System halt requested...` and leave a tab with no prompt, which could only be closed.
- In the first tab, the question asked when its shell ends reaches the screen, and a key answers it: `y` exits the app, and anything else starts a new shell. It went only to a buffer the terminal doesn't draw, so the tab showed a cursor with no prompt, and any key answered a question nobody could see.
- A redirection on a closed stdout, stderr or stdin leaves it closed. `( exec >&-; { :; } > f; echo x )` printed `x`, because what was saved and put back was the host's descriptor of that number.
- `>&$fd` with a closed descriptor of 10 or above says `Bad file descriptor`. It could find the copy of stdout saved for that very redirection in the same slot, and write to stdout.

## Fixed in exsh on every platform
- `{ ...; } > file`, `if ...; fi > file`, `case ... esac > file` and a redirection on a function definition now cover the whole body, including input redirections, and they close when the body leaves early through `return`, `break` or `exit`. They used to be dropped: the output went to the terminal and the file was never created.
- A redirection on a compound command that cannot be opened now fails that command with status 1 and skips its body, and the script goes on.
- What exsh starts sees only exported variables, as in bash: `x=1; env` no longer lists `x`, and a big variable no longer makes later commands fail with `Argument list too long`. On iOS the same goes for applets, frontends and scripts, `X=1 cmd | ...` shows `X` only to `cmd`, and a background `X=1 cmd &` sees it. What a program sets stays with that program.
- `y=$(echo "$v")`, `( ... )`, `{ ...; } | cat` and `&` inside a function see the function's locals. A substitution came back empty on every platform.
- `wait` finds any job this shell started, however long ago it ended, in any order and as often as asked. `( exit 3 ) & a=$!; ( exit 4 ) & b=$!; sleep 0.2; wait $a` gave 127 and printed `invalid job` twice. A number is now a pid and `%n` a job, several operands work, and `wait` with no operands gives 0 and forgets every job. As in bash, a prompt reports what has ended: its `%n` goes and `wait -n` skips it, and its pid still works.
- A line typed while a command runs now runs at the next prompt, and lines pasted together all run. On a plain build the typed line was lost, and of three pasted lines only the first ran. Input a program leaves unread reaches the prompt too, as in bash.
- `$(...)` acts as a subshell in more ways. `x=$(exec cmd)` ends only the substitution, where on a host it replaced the whole shell, and `exec 3> f` in one ends with it. `z=$( { sleep 0.2; echo late; } & echo early )` gets both lines, because it reads to end of file, so it waits for a job that still holds its output and not for one whose output goes elsewhere. On a host `w=$(exec 4> f; exec 5> g; echo hi)` no longer loses all stdout for the rest of the script, and a job started in one no longer holds the outer stdout.
- `printf` no longer cuts a number or `%c` at 255 characters: `printf '%300d' 7` prints all 300.
- `printf`'s `%n` stores what this pass over the format has written, in the variable named by its argument. A letter `printf` does not know fails with status 1 and stops there: `%D` printed a string's address and `%y` printed `y` with status 0. `%S` and `%C` are `%s` and `%c`, as in bash 5.
- A failed expansion in a background command or a later stage of a pipeline is that command's failure, as in bash: it reports and ends with status 1, the rest of the pipeline runs and the script goes on.
- `$1` under `set -u` is reported as `$1: unbound variable`, as bash has it.
- A DEBUG trap no longer runs for a compound stage or a backgrounded compound command, as in bash.
- `x=$(umask 077)` no longer changes the shell's mask.
- `<>` opens for reading and writing and makes the file. It was treated as `<`, so `exec 3<> fifo` hung.
- Every builtin that prints reports a failed write. With stdout closed, `printf`, `pwd`, `type`, `declare -p` and others gave 0 and left their text for the next flush, wherever stdout was by then, so `exec >&-; printf lost; exec > f` put it in `f`.
- `return` ends the loops and cases it leaves, as a statement, in a pipeline stage or in a background body. In an ERR or signal trap it ends the function that trap broke into, and `eval return` outside a function says so and goes on.
- Bad arguments to `return` and `break` do what bash does, `set -E`, `set -T` and `declare -ft` work, `time` is a keyword that times a function too, and `{ exit 3; } > o` keeps the redirection for the EXIT trap.
- `set` and `declare` alone list the variables, prefix assignments go in order, a readonly prefix is reported and the command runs, `unset` of a local keeps the outer variable hidden, a trap runs as soon as the command is done, and `read` follows bash on trailing whitespace.
- A signal sent to the shell while `$(...)` runs now waits for it, and the shell's trap runs after the command, as in bash. `x=$(cmd)` assigns, then the trap runs. The trap used to run inside the substitution: its `echo` landed in the variable and its `exit` ended only the substitution, so `trap 'exit 1' TERM; while :; do x=$(echo 1); done` could not be stopped by TERM, and a script went on to the end with status 0.
- `trap - TERM` or `trap '' TERM` inside `$(...)` stays there. It used to change the signal for the whole shell, which could then die of a TERM it had a trap for.
- Ctrl-C during `$(...)` with an INT trap set ends the substitution, and the trap runs afterwards.
- The EXIT trap runs when a signal's trap exits inside `eval` or a sourced file in a loop.
- A trap's `exit` at a loop's own check ends a subshell or function there. The rest of it used to run, and its status replaced the trap's.
- A foreground command dying of a signal no longer runs the shell's trap for that signal, except for Ctrl-C, Ctrl-\ and Ctrl-Z.
- On iOS, a signal with no trap sent to the shell during `$(...)` ends the shell, not only the substitution.
- `tools/shellbench_cacheable/shellbench` gets a number from exsh on every case of `sample/assign.sh` and `sample/eval.sh`.
- `kill SIG 0` signals the shell's own group, on every platform. It said `kill: (0) Invalid argument`.
- A loop, `if`, `{ }` or `( )` with no command in it is a syntax error, as in bash: `while false; do done`, `if true; then fi`, `{ }`. They used to run as if nothing was wrong, so a script holding one now stops there with status 2. Case arms may still be empty.
- A token no command can start with is reported in bash's words, with the line it is on: ``syntax error near unexpected token `done'``. `while do`, `{ ; }` and a stray `done` used to say `shell parse error at 1:9: Expected command`. Other parse errors keep exsh's own wording.
- `exec {fd}> f` and every other `{name}` redirection work, on `exec`, a builtin, a function and a compound command: the shell picks a descriptor from 10 up, `name` (an array element too) gets its number, and it stays open until `exec {name}>&-`. It used to look for a program called `{fd}`.
- `wait -n` in `$(...)` gives 0 for each job the shell started, then 127, as in bash. It gave 127 at once.
- `exec ""` says `exec: : not found` and ends the script with status 127. It said `exec: expected command`, status 1, and went on.
- On a host, `exec 3< <(cmd)` and `exec 3> >(cmd)` keep descriptor 3, and the same goes for any other number the pipe could land on. It was closed when `exec` was done, so the next read said `Bad file descriptor`.
- `${!:-x}` and the other operators right after the `!` mean `$!` with that operator. They said `bad substitution`. `${!!}` and `${!$}` are now bad substitutions, as in bash, and `$!` with no job yet fails under `set -u`.
- `/bin/x` and `/usr/bin/x` on a host run the file, not the builtin of that name, as in bash. `/bin/echo -e` gets the file's answer, `/bin/cd` no longer moves the shell, and `/bin/exit` no longer ends it.
- `which` on a host is the system's program, as in bash, so `t=$(which true); "$t"` works. The app keeps its builtin.
- A program started by `command` keeps its own name: `command sh -c 'echo "$0"'` prints `sh`. `command -p` still passes the path.


## Faster in exsh
- Commands that stay in the shell take about 0.41 of the time they did, which is about 1.65x bash 5 where it was about 4x. The numbers are from `tools/shellbench_cacheable`, 3 rounds on one laptop, with a Release build of each version. Commands that start a process are unchanged, at about 0.9x bash 5.
- `[ ]` takes 3.7 µs, down from 21.4 µs, and no longer reads the directory. Among 10,000 files it took 2,911 µs and takes 4.1 µs.
- `eval` takes 9.2 µs, down from 49.3 µs, and a function call 4.0 µs, down from 10.7 µs. `eval`, `source` and traps keep one VM between calls, as functions already did. A reset skips thread slots nobody used, and builtin names are found by hash.
- The shell asks once whether its ids can be trusted, where it made about 20 system calls per command. It asks again after an applet, since `su`, `sudo`, `login` and `chroot` come back with new ids if their exec fails.
- A word with a `[` and no `]` after it in the same part of a path stays as it is, as in bash. That makes these work as they should: `shopt -s nullglob; [ a = a ]` failed with `a: No such file or directory`, `failglob` stopped on `[`, `dotglob` with a file `.[` turned `[` into `.[`, and `nocaseglob` with a file `A[` turned `a[` into `A[`.


## Fixed in rsync
- `rsync file newname` and `rsync host:file newname` make a file named `newname` when nothing is there, as GNU `rsync` does. They used to make `newname/file`. A destination ending in `/`, an existing directory or more than one source still means a directory.
- `rsync file oldfile` replaces the file. It used to fail, as it did with a link there. A link or a FIFO in the way goes, and the link's target stays as it was. With `dir/file` a link, `rsync file dir` failed, and with a FIFO there it blocked for good.
- An updated file keeps its own permissions unless `-p` is given. It used to take the source's.
- Nothing is made until the file list arrives, so a missing source, a directory without `-r` or an `--exclude` that matches everything leaves no empty directory behind.
- A destination that can't be used ends with status 3: a file where a directory is needed, or a missing directory above a new name. `rsync file nosuch/new` used to make `nosuch/new/file`. `rsync -n` says so too.


## For iOS-flavoured builds on a host
- The real root is never provisioned. `PATH_TRUNCATE=/`, any spelling or symlink that comes to it, and `HOME=/` with nothing else set used to make directories in the root, prune `/proc` there and rename links over `/usr/bin` names. `$HOME` stands in now, as path translation already did, and nothing is provisioned when `$HOME` is the root or unset.


## Differs from bash
- `kill -INT $$` from inside `$(...)` with an INT trap set ends the substitution too, where bash's would carry on, because exsh's runs in the shell's process.
- A substitution that sets its own trap takes that signal even when it was sent to `$$`, for the same reason.
- Every child gets a process group of its own, with job control off too, where bash leaves a script's children in the shell's group. So `kill 0` in a script reaches only the shell, and `kill -- -$!` finds the job where bash says no such process.
- A syntax error stops a script before any of it runs, where bash runs the lines before it.
- `a; ;` still parses, where bash reports the second `;`.
- The words of a command's redirections expand before any of them acts, so `exec {a}> f {b}>&$a` finds `a` empty.
- `command prog {fd}> f` sets `fd` in the shell, and the descriptor stays open there.
- `shopt varredir_close` is not there.


## Differs from GNU rsync
- Missing directories above a destination that means a directory get made: `rsync -a dir x/y/new`. GNU's fails with status 11 unless given `--mkpath`. The rsync in macOS makes them too.
- A new directory without `-p` gets the default mode, where GNU's gets the source's.
- A missing source ends the transfer with status 1, where GNU's copies the rest and ends with 23.
- `-c` fails or stops at the timeout with a GNU `rsync` at the other end.
- A device at the destination is refused with status 3. GNU's replaces it as root, and otherwise ends with 23.


## Differs from Linux on iOS
- `exec 3> >(cmd)` and `exec > >(cmd)` run the command at once on an empty file, so what the script writes afterwards is lost.
- A function given `{fd}> f` as a job or a pipeline stage does not see `fd`.
- `halt`, `reboot` and `poweroff` end only their own tab, since the app can't stop its device and each tab is a session of its own. They don't stop that tab's other commands: a job goes on as it does after `exit`, and the tab closes when a command in the same pipeline has finished.
- A signal that ends a shell waiting at its prompt, from a job or another tab, takes effect when the next line is typed, where Linux ends it at once. So `halt &` closes the tab at the next Return, and that line doesn't run.
- A fatal signal a subshell sends itself returns: `( kill -TERM $BASHPID; echo x )` prints `x`, though its status is 143.
- A signal to a task that has ended but not been waited for says `No such process`, where Linux says 0.
- SIGINT or SIGTSTP to your own group on your own thread is queued for the next check, for programs that check, where Linux acts before `kill` returns.
- The task table has no user id, so anyone can signal anyone. That is Linux for one user, and `EPERM` belongs here once the app has users.
- `which true` prints `true: shell builtin`, so running its answer gives status 127. `/bin/cd` moves the shell, `/bin/export` and `/bin/set` act on it, and `/bin/exit` ends it, where Linux gives status 127. No file exists for those names.

## Known Issues
Shell, on iOS:
- A failure as the last command on a prompt line does nothing under `set -e` or an ERR trap. `set -e` then `false; echo x` ends the shell, and `set -e` then `false` does not.
- A file run with `.` at a prompt announces its background jobs, `[1] 123` on stderr. bash announces only what is typed at the prompt.
- `: ${v:=1} &` and `echo $((v=5)) &` still change the shell, on every platform, because words expand before the job starts.
- Which stdin a background job gets differs from bash: `{ r & wait; } < f` gives the job `/dev/null` where bash gives it `f`.
- `dirs` in a pipe shows one entry, and `jobs | wc -l` in a nested shell lists the caller's job. Through a file both are right.
- `echo x > in` is a parse error, as is any other reserved word as the target. bash takes it as a file name. In a sourced file that drops the rest of the file.
- `which true` prints `true: shell builtin`, so `t=$(which true); "$t"` is 127. `/bin/cd /` and `/bin/exit 3` act on the shell, where Linux has no such files.
- `/dev/stdout` and `/dev/stderr` are regular files under the app's root, and `cat /dev/stdin` says no such file. `/dev/null`, `/dev/zero`, `/dev/urandom` and `/dev/tty` are fine.
- `cat < /dev/tty` opens the real device, not the app's terminal, and fails.
- `history` ignores its arguments, so `history -c` clears nothing.
- Not there: `select`, `compgen`, `complete`, `compopt`, `suspend`, `declare -n`, `printf '%(fmt)T'`, and `ulimit` in the app.

Commands, on iOS:
- A CLike program looping on `eof(stdin)` and `readln` at the keyboard loses what is typed and doesn't end on Ctrl-D. From a file or pipe it works.
- After killing a CLike program blocked in `eof(stdin)` at the keyboard, the next such program got nothing typed and the line after it didn't run. This affects later stdin readers, not the whole app.
- Ctrl-C during `eof(stdin)` at the keyboard gives status 1, not 130.
- A builtin writing to a full FIFO, such as `echo` with a large argument, can't be interrupted when killed. `wait` reports it gone after 2 s, but its thread stays until a reader drains the FIFO.
- `sftp`'s `lumask` still sets the whole app's mask, and so can `ssh` and `scp -p`. Files a task creates through the shell's paths keep its own mask when the app's is looser, and come out tighter when it is stricter.
- A program that creates files with its own libc calls, past the shims, obeys the session shell's mask, not its task's.
- `wc -m` counts bytes unless a locale is set, because `LANG` is not in the app's environment by default.
- `rsync host:file newname` and `rsync file newname` make `newname/file` where GNU `rsync` makes a file `newname`, unless the receiving side is another `rsync`.
- `ssh a cmd | ssh b cmd` can hang when it moves much data. Other pairs of commands polling at the same time may stall too.
- `ssh -f` isn't supported: it ends with status 255 and `daemon() failed`.
- ^C does not stop `rsync` to or from a remote host, which starts its `ssh` directly. A background `ssh` can take a ^C meant for the foreground, because the interrupt flag is process-wide.
- ^C on a Pascal `readln` ends it, but prints a VM debug dump. `delay()` ignores ^C and takes the keys that follow. `wc` and `sort` print a result on ^C. `cat | tr` ended by ^C keeps status 0, so the rest of that line still runs.
- `wait -n` does not see a job collected on an earlier prompt line.
- `who am i` prints nothing at a prompt, because nothing in the app writes a utmp record for a terminal. `who -m file` finds the line in a file that has a record for it.
- `lps` keeps an `exited` row for a frontend that `timeout` killed, until the app restarts. It holds no thread or descriptor. With `( pascal spin ) & kill $p` the row says `exited` while the program goes on running, and `kill` can't reach it.
- `halt`, `reboot` and `poweroff` without `-w` print `System halt requested...` and end the shell, which leaves its tab open with no prompt. Open a new tab. Build 31 ends the shell the same way, without the message.
- The terminal's size can be wrong after an SDL program exits, and `reset` doesn't restore it. Seen on an iPad, not reproduced in the simulator.
- `aether --version` reads `1.01-dev` in this build. The language is the frozen 1.0 one; the label comes from `components/aether/RELEASE`, which moved on right after the 1.0 tag.
- The app bundles no Aether guide: `~/Docs` has the PSCAL documents only.
- Variables cross tabs: `x=1` in one tab shows in another. An `unset HOME` or `export HOME=x` in one tab can make a path in another tab go wrong.
- A killed last stage of a pipeline still gives `$?` 0. Each foreground command killed from another task can leave an `exited` row in `lps` and an open descriptor; a loop reached `Too many open files` after about 40 kills.
- A DEBUG trap never runs inside a function or `$(...)`.
- Text written to `/dev/tty` doesn't show on screen, and `/dev/tty` fails in the first tab after a second tab has closed.
- `halt &` waits for the next line before the tab closes. With stderr redirected, an `ssh` or `ssh-keygen` prompt goes to the file and not the screen; typing the answer still works.
- Ctrl-C may not end a `$(...)` that is waiting for a background job still holding its pipe. Seen in the host test of the iOS build, not checked in the app.

## Validation
At umbrella `eaec0eef` (pscal-core 206faae, exsh 82bb740, smallclue 1d5f9ea), CI green. The simulator runs used a Release build from Xcode 26.6 with the iOS 26.5 SDK, on iOS 26.5 and 27.0 simulators; the archive for TestFlight is built with Xcode 27.1.
- Host suites from clean submodules: exsh, pascal, clike, rea, aether, scope and `run_all_suites` pass; the iOS-flavoured list passes; bash 3.2 parity has only the known failures. The interactive harness fails the same tests as at the previous candidate, except `interactive_ctrl_c_substitution_wait`, a new test listed under Known Issues.
- Typing at the real prompt: 153 passed, 0 failed.
- Upgrade over build 31, the danger probes, keys, tabs, `wait` on killed jobs and the Known Issue probes: as expected.
- App smoke, 858 rows each run: iPad iOS 26.5 30 of 30 runs, iPhone iOS 26.5 10 of 10, iPad iOS 27.0 10 of 10, every row passing, no crash reports.
