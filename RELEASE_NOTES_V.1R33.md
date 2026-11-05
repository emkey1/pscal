# PSCAL V.1R33 Release Notes

Base: `V.1R32`

## Fixed in exsh
- A failed expansion in a background command or a later stage of a pipeline is that command's failure, as in bash: it reports and ends with status 1, the rest of the pipeline runs and the script goes on. `set -u; echo $nope &` and `true | cat ${v:?msg}` used to end the script.
- `$1` under `set -u` is reported as `$1: unbound variable`, as bash has it.
- A DEBUG trap no longer runs for a compound stage or a backgrounded compound command, as in bash: `{ a; } | b` and `( a ) &` counted one too many.
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
- Every other syntax error is reported in bash's words too, with the line it is on: a keyword where another has to come (`if true; done`), a redirection with no target (`echo >`), a bad case pattern (`case x in )`), a doubled `;` or `&`, and a word after a compound command (`( a ) b`, `if a; then b; fi c`). Several of these used to run.
- The lines before a syntax error run, as under bash, which reads a line at a time. exsh used to run none of the script. An `exit` or `return` before the error means nothing reports it.
- Text that ends inside a quote, `$(`, `${` or an array is an error, `unexpected EOF while looking for matching`, with status 2, or 1 for an array. It used to run as if the quote were closed. Text that ends inside `if`, a loop or a group says `syntax error: unexpected end of file`.
- A here-document that ends with the text runs with what it has, with bash 5's warning. It used to be an error.
- A reserved word works wherever only a word can stand: `for in in in`, `for x in do`, `case if in if)`, `echo x > in`.
- `f() (( 1 ))` and `f() [[ a ]]` define functions, and `if (a) then (b) fi` and `while (a) do (b) done` run. `f() echo x` is an error, where it used to define nothing and say `invalid stage count`.
- `for 1 in a` reports `` `1': not a valid identifier `` when the loop runs, with status 1, and the script goes on, as under bash. It used to be a syntax error.
- At a prompt, `for` or `echo >` followed by Enter reports the error at once instead of asking for another line, and `!` alone or `! cmd` is no history reference. `!` alone is `! :`, as in bash 5.
- `exec {fd}> f` and every other `{name}` redirection work, on `exec`, a builtin, a function and a compound command: the shell picks a descriptor from 10 up, `name` (an array element too) gets its number, and it stays open until `exec {name}>&-`. It used to look for a program called `{fd}`.
- `wait -n` in `$(...)` gives 0 for each job the shell started, then 127, as in bash. It gave 127 at once.
- `exec ""` says `exec: : not found` and ends the script with status 127. It said `exec: expected command`, status 1, and went on.
- On a host, `exec 3< <(cmd)` and `exec 3> >(cmd)` keep descriptor 3, and the same goes for any other number the pipe could land on. It was closed when `exec` was done, so the next read said `Bad file descriptor`.
- `${!:-x}` and the other operators right after the `!` mean `$!` with that operator. They said `bad substitution`. `${!!}` and `${!$}` are now bad substitutions, as in bash, and `$!` with no job yet fails under `set -u`.
- `wait -n ID` waits for the jobs it names. It ignored them and took whichever ended first. `wait` also takes `-p name`, which gets the pid, and `-f`, and a word it does not know as an option is an error, status 2.
- A redirection's word expands when its turn comes where that can show: `exec {a}> f {b}>&$a` uses the descriptor just made, and `echo y > a 2> "$(cat a)"` reads `a` after it is emptied. Nothing after a redirection that fails runs.
- `command prog {fd}> f` leaves the shell as it was, and the descriptor closes with the program, as on a plain `prog {fd}> f`.
- `shopt -s varredir_close` closes a `{name}` descriptor when its command ends, unless that is `exec`.
- `${!@}` and `${!*}` use the positional parameter as the name, with any operator after them applied to that. They gave nothing. Several parameters, or one that is no name, is `invalid variable name`, as in bash 5.
- `[[ 3>2 ]]` and `[[ {a}>b ]]` are syntax errors, as in bash 5. A number or `{name}` touching the operator is a redirection there. They ran as a comparison.
- `wait -n` in `<(...)` and `>(...)` on a host gives 0 for each job the shell started, then 127, as it does in `$(...)`. It gave 127 at once. `jobs` there lists them.
- On iOS, a function, `eval` or `.` given `{fd}> f` as a job or a pipeline stage sees `fd`.
- `/bin/x` and `/usr/bin/x` on a host run the file, not the builtin of that name, as in bash. `/bin/echo -e` gets the file's answer, `/bin/cd` no longer moves the shell, and `/bin/exit` no longer ends it.
- `which` on a host is the system's program, as in bash, so `t=$(which true); "$t"` works. The app keeps its builtin.
- A program started by `command` keeps its own name: `command sh -c 'echo "$0"'` prints `sh`. `command -p` still passes the path.
- A command line that wraps shows once. Under a prompt with a glyph like the default prompt's `⚡`, the prompt and the line's first row printed again at each wrap, because the editor counted that glyph as no cells and the terminal as two. The editor now counts as the terminal does, whatever the locale, and its screen matches bash's in a UTF-8 locale.
- A line that ends exactly on the last column no longer eats the row above the prompt at the next key, and Enter there leaves no empty row. Neither does a key pressed with the cursor on an earlier row of a long line.
- Enter, Ctrl-C and a completion list with the cursor on an earlier row of a long line print below all of it, not over its end. A reverse search with a match that wraps draws over itself.
- Left, Right, Backspace and Delete at the prompt take a whole character, with any mark after it, where they took one byte.
- `[` and `test` compare strings. A right side of `=`, `==` or `!=` holding `*`, `?` or `[` was read as a pattern, so `[ abc = "a*" ]` was true, `[ "[5]" = "[5]" ]` was false, and `while [ "$x" != "[5]" ]` never ended. `nocasematch` no longer reaches them either. `[[ ]]` and `case` keep both.
- A value of `!` is a value: `v='!'; [ "$v" = x ]` was true. So is a value that looks like an operator in a longer expression, such as `[ "$v" = x -a 1 = 1 ]` with `v=-n`, and `)` as a word.
- `[` and `test` with the wrong number of words give bash's message and status 2, also as a `while` or `until` condition. `[ $empty = y ]` and `[ a b ]` used to be false with no message.
- `[ -a f ]`, `[ -o noglob ]`, `[ -v x ]`, `[[ -a f ]]`, `[[ -o noglob ]]` and `builtin [` work. The first two were always false, and `builtin [` said `unknown VM builtin`.

## Faster in exsh
- Commands that stay in the shell take about 0.41 of the time they did, which is about 1.65x bash 5 where it was about 4x. The numbers are from `tools/shellbench_cacheable`, 3 rounds on one laptop, with a Release build of each version. Commands that start a process are unchanged, at about 0.9x bash 5.
- `[ ]` takes 3.7 µs, down from 21.4 µs, and no longer reads the directory. Among 10,000 files it took 2,911 µs and takes 4.1 µs.
- `eval` takes 9.2 µs, down from 49.3 µs, and a function call 4.0 µs, down from 10.7 µs. `eval`, `source` and traps keep one VM between calls, as functions already did. A reset skips thread slots nobody used, and builtin names are found by hash.
- The shell asks once whether its ids can be trusted, where it made about 20 system calls per command. It asks again after an applet, since `su`, `sudo`, `login` and `chroot` come back with new ids if their exec fails.
- A word with a `[` and no `]` after it in the same part of a path stays as it is, as in bash. That makes these work as they should: `shopt -s nullglob; [ a = a ]` failed with `a: No such file or directory`, `failglob` stopped on `[`, `dotglob` with a file `.[` turned `[` into `.[`, and `nocaseglob` with a file `A[` turned `a[` into `A[`.

## Fixed in the iOS app
- `env test a == a`, and `xargs` or `find -exec` running `test`, know `==`, and take a value of `!`. They said `test: unsupported expression`.
- A pipeline with a DEBUG trap set no longer ends the app. A trap on the shell's thread cleared the pipeline's state while a stage's thread was reading it. A trap now runs in a copy of the shell's state while a pipeline runs, whichever trap it is.
- A failed expansion in a later stage of a pipeline no longer ends the app. `set -u; f() { sleep 1 | cat $nope; }; f` ended the shell with the first stage still starting, most of the time in a function, `eval` or a sourced file.
- `kill -TERM -1` means what it means on Linux: every task but the caller and the app's kernel task, and `No such process` when none is left. It used to mark the kernel task as dying and cancel its thread, and `kill -0 -1` said a task was there when none could be reached.
- A signal to the app's kernel task succeeds and does nothing, as for init on Linux. `kill -TERM 1` could stop the thread that carries Ctrl-C.
- A signal stays in the app, from any thread and whatever the pid. From a thread with no task of its own, `0`, `-1` and `-N` went to iOS's `kill`, which reads them as real process groups, and `-1` as every process the app may signal. A positive pid went there too with a signal such as SIGKILL, SIGHUP or SIGUSR1, so it reached whichever process had that number and not the task. Every pid now means the app's own tasks.
- `kill(0, sig)` from a program acts as `kill(-pgid, sig)` does. SIGINT to your own group is queued for the next check in both forms. `kill(0, SIGINT)` used to mark the task dead while it ran on.
- `halt`, `reboot` and `poweroff` without `-w` end the tab, as `exit` does, from the prompt, a script, a pipeline or a subshell. In the first tab they ask what `exit` asks, `This will exit the app. Are you sure?`. They used to print `System halt requested...` and leave a tab with no prompt, which could only be closed.
- In the first tab, the question asked when its shell ends reaches the screen, and a key answers it: `y` exits the app, and anything else starts a new shell. It went only to a buffer the terminal doesn't draw, so the tab showed a cursor with no prompt, and any key answered a question nobody could see.

- A redirection on a closed stdout, stderr or stdin leaves it closed. `( exec >&-; { :; } > f; echo x )` printed `x`, because what was saved and put back was the host's descriptor of that number.
- `>&$fd` with a closed descriptor of 10 or above says `Bad file descriptor`. It could find the copy of stdout saved for that very redirection in the same slot, and write to stdout.

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

- Unquoted parentheses among a command's words are plain words: `find . ( -name a )` runs, and `f(x) { :; }` reports `}` where bash reports `x`. The owner chose to keep this.
- A `[[ ]]` bash can't parse, such as `[[ a == ]]`, runs.
- Text that runs out says what 3.2 and 5.2 say. 5.3 adds which construct was open, and where.

## Differs from GNU rsync
- Missing directories above a destination that means a directory get made: `rsync -a dir x/y/new`. GNU's fails with status 11 unless given `--mkpath`. The rsync in macOS makes them too.
- A new directory without `-p` gets the default mode, where GNU's gets the source's.
- A missing source ends the transfer with status 1, where GNU's copies the rest and ends with 23.
- `-c` fails or stops at the timeout with a GNU `rsync` at the other end.
- A device at the destination is refused with status 3. GNU's replaces it as root, and otherwise ends with 23.

## Differs from Linux on iOS
- `exec 3> >(cmd)` and `exec > >(cmd)` run the command at once on an empty file, so what the script writes afterwards is lost.
- `halt`, `reboot` and `poweroff` end only their own tab, since the app can't stop its device and each tab is a session of its own. They don't stop that tab's other commands: a job goes on as it does after `exit`, and the tab closes when a command in the same pipeline has finished.
- A signal that ends a shell waiting at its prompt, from a job or another tab, takes effect when the next line is typed, where Linux ends it at once. So `halt &` closes the tab at the next Return, and that line doesn't run.
- A fatal signal a subshell sends itself returns: `( kill -TERM $BASHPID; echo x )` prints `x`, though its status is 143.
- A signal to a task that has ended but not been waited for says `No such process`, where Linux says 0.
- SIGINT or SIGTSTP to your own group on your own thread is queued for the next check, for programs that check, where Linux acts before `kill` returns.
- The task table has no user id, so anyone can signal anyone. That is Linux for one user, and `EPERM` belongs here once the app has users.
- `which true` prints `true: shell builtin`, so running its answer gives status 127. `/bin/cd` moves the shell, `/bin/export` and `/bin/set` act on it, and `/bin/exit` ends it, where Linux gives status 127. No file exists for those names.
