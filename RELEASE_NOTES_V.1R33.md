# PSCAL V.1R33 Release Notes

Base: `V.1R32`

## Fixed in exsh
- IFS splits only what an expansion produced, as in bash. Text a word itself holds used to split too: `IFS=:; printf '<%s>' a:b` gave two fields, `IFS=/; ls /dev/null` passed `dev` and `null`, and `IFS=e; echo hello` could not find its command. Redirection targets, `for` lists, loop conditions and the words inside `$(...)` all follow it. Where bash 3.2 and 5 differ, as in `a:$x:b`, exsh follows 5.
- A quoted part no longer keeps the rest of a word from being split or matched: `"c"$x` with `x='a b'` is two fields where it was one, and `""$g` with `g='f*'` matches files.
- `$@`, `$*` and `${a[@]}` outside quotes are joined with IFS's first character and then split, as in bash, so `set -- a '' b; printf '<%s>' $@` gives `<a><b>` and under `IFS=:` gives `<a><><b>`. The first used to keep the empty one and `IFS=:; set -- a: b` lost one.
- White space around an IFS delimiter is part of it: with `IFS=' :'`, `a : b` is two fields where it was three and `a : ` is one where it was two. A field that starts with a quoted or literal byte ends at its delimiter: `x$y` under `IFS=:` gave an empty field for each.
- `""$@` and `$@""` with no arguments are one empty field, and `IFS=; x=; printf '%s' $x` gives no field where it gave one.
- `> $@` with two arguments is an ambiguous redirect, as in bash. It used to write a file named `a b`.
- `while [ f* = fa ]` and `until test ...` expand their words as any command does: a loop's test used to skip braces and globs.
- A glob's matches sort as the locale collates them, as in bash, by `LC_ALL`, `LC_COLLATE` or `LANG`, exported or not, from the moment one is assigned. Under `en_US.UTF-8` on Linux `echo *` gives `[a] a b [x Z`; it used to sort by bytes under every locale.
- `sh /dev/stdin` reads a script from a pipe, on every platform. It failed with status 126 and said nothing.
- Every other syntax error is reported in bash's words too, with the line it is on: a keyword where another has to come (`if true; done`), a redirection with no target (`echo >`), a bad case pattern (`case x in )`), a doubled `;` or `&`, and a word after a compound command (`( a ) b`, `if a; then b; fi c`). Several of these used to run.
- The lines before a syntax error run, as under bash, which reads a line at a time. exsh used to run none of the script. An `exit` or `return` before the error means nothing reports it.
- Text that ends inside a quote, `$(`, `${` or an array is an error, `unexpected EOF while looking for matching`, with status 2, or 1 for an array. It used to run as if the quote were closed. Text that ends inside `if`, a loop or a group says `syntax error: unexpected end of file`.
- A here-document that ends with the text runs with what it has, with bash 5's warning. It used to be an error.
- A reserved word works wherever only a word can stand: `for in in in`, `for x in do`, `case if in if)`, `echo x > in`.
- `f() (( 1 ))` and `f() [[ a ]]` define functions, and `if (a) then (b) fi` and `while (a) do (b) done` run. `f() echo x` is an error, where it used to define nothing and say `invalid stage count`.
- `for 1 in a` reports `` `1': not a valid identifier `` when the loop runs, with status 1, and the script goes on, as under bash. It used to be a syntax error.
- At a prompt, `for` or `echo >` followed by Enter reports the error at once instead of asking for another line, and `!` alone or `! cmd` is no history reference. `!` alone is `! :`, as in bash 5.
- `wait -n ID` waits for the jobs it names. It ignored them and took whichever ended first. `wait` also takes `-p name`, which gets the pid, and `-f`, and a word it does not know as an option is an error, status 2.
- A redirection's word expands when its turn comes where that can show: `exec {a}> f {b}>&$a` uses the descriptor just made, and `echo y > a 2> "$(cat a)"` reads `a` after it is emptied. Nothing after a redirection that fails runs.
- `command prog {fd}> f` leaves the shell as it was, and the descriptor closes with the program, as on a plain `prog {fd}> f`.
- `shopt -s varredir_close` closes a `{name}` descriptor when its command ends, unless that is `exec`.
- `${!@}` and `${!*}` use the positional parameter as the name, with any operator after them applied to that. They gave nothing. Several parameters, or one that is no name, is `invalid variable name`, as in bash 5.
- `[[ 3>2 ]]` and `[[ {a}>b ]]` are syntax errors, as in bash 5. A number or `{name}` touching the operator is a redirection there. They ran as a comparison.
- `wait -n` in `<(...)` and `>(...)` on a host gives 0 for each job the shell started, then 127, as it does in `$(...)`. It gave 127 at once. `jobs` there lists them.
- On iOS, a function, `eval` or `.` given `{fd}> f` as a job or a pipeline stage sees `fd`.
- A command line that wraps shows once. Under a prompt with a glyph like the default prompt's `⚡`, the prompt and the line's first row printed again at each wrap, because the editor counted that glyph as no cells and the terminal as two. The editor now counts as the terminal does, whatever the locale, and its screen matches bash's in a UTF-8 locale.
- A line that ends exactly on the last column no longer eats the row above the prompt at the next key, and Enter there leaves no empty row. Neither does a key pressed with the cursor on an earlier row of a long line.
- Enter, Ctrl-C and a completion list with the cursor on an earlier row of a long line print below all of it, not over its end. A reverse search with a match that wraps draws over itself.
- Left, Right, Backspace and Delete at the prompt take a whole character, with any mark after it, where they took one byte.
- `[` and `test` compare strings. A right side of `=`, `==` or `!=` holding `*`, `?` or `[` was read as a pattern, so `[ abc = "a*" ]` was true, `[ "[5]" = "[5]" ]` was false, and `while [ "$x" != "[5]" ]` never ended. `nocasematch` no longer reaches them either. `[[ ]]` and `case` keep both.
- A value of `!` is a value: `v='!'; [ "$v" = x ]` was true. So is a value that looks like an operator in a longer expression, such as `[ "$v" = x -a 1 = 1 ]` with `v=-n`, and `)` as a word.
- `[` and `test` with the wrong number of words give bash's message and status 2, also as a `while` or `until` condition. `[ $empty = y ]` and `[ a b ]` used to be false with no message.
- `[ -a f ]`, `[ -o noglob ]`, `[ -v x ]`, `[[ -a f ]]`, `[[ -o noglob ]]` and `builtin [` work. The first two were always false, and `builtin [` said `unknown VM builtin`.
- Failed arithmetic names what bash names: the expression as it was read, without blanks in front, and the last token read with all that follows it. `$((1/0))` says `1/0: division by 0 (error token is "0")`, `2+1/0` the same `"0"`, `1/x` says `"x"`, `$(( 1 / 0 ))` says `1 / 0 : ... "0 "` and `1/0+1` says `"0+1"`, as bash 5 does. `$((1/$x))` shows `1/0`, and a variable holding `1/0` shows that, not the variable's name. The token used to be the text after the cursor, usually `""`.
- `((1/0))` reports once and runs once. It used to fall through to a second evaluation, so `((x++, 1/0))` raised x by 2 and printed the message twice.
- `$((1/0))`, `${a[1/0]}`, `${x:1/0}`, `a[1/0]=1` and `declare -i n=1/0` drop the rest of their line and the script reads on from the next, with status 1, as in bash. They used to end the script. So does eval and a sourced file, from their own next line. A subshell or `$(...)` still ends.
- `for ((;;))` takes each clause as one expression. It split the condition at the first comparison it found, so `i<3 ? 1 : 0`, `i<3<2` and `! i > 1` gave wrong answers. A clause that fails is reported as `((: i=1/0: ...`, where it used to say nothing, and `set -e` does not take it for a command that failed.
- `[[ 1/0 -eq 1 ]]` reports the failure, is false and has status 1. It said nothing and had status 2, and the rest of `[[ ]]` still runs, as `[[ 1/0 -eq 1 || 2 -eq 2 ]]` shows.
- `declare -i n=1/0`, `typeset -i` and `local -i` report the real error under their own names. They said `syntax error: operand expected` for all of them, from a check that ran the system's bash to choose its words, which is gone. `declare -i n=5; n=1/0` and `n+=1/0` report and keep 5, where they silently set 0. `declare -i n` and `local -i n` start empty, not 0.
- `let`, `[[ ]]` and `declare -i` do not expand their text again: `let '$x+1'` and `[[ '$x' -eq 1 ]]` are errors, as in bash. Double quotes are dropped in `$(( ))` and `(( ))`, not in `let` or a variable's value, where `x='"1"'; echo $((x))` is an error.
- `set -u; ((nosuch))`, and `let`, `[[ ]]` and `for ((;;))` with an unset name, end the script, as `$((nosuch))` always did. The message is `nosuch: unbound variable`, and `r: readonly variable` has no expression or label either.
- More of bash's arithmetic errors: `1 = 2` is `attempted assignment to non-variable`, `1 @ 2` is `invalid arithmetic operator`, `1?` and `1?2:` are `expression expected`, `2#` is `invalid integer constant`. `0x` is 0. `a?b:c=1` is an error, since the third part is no assignment, and `x**=2` is not one either.
- `a=([1+1]=x)` sets index 2. Outside `declare -A` a subscript in a literal is arithmetic, where it became a key named `1+1`. `a=([1/0]=1)` reports and leaves the array empty.
- `${x:1/0}` with x unset, and `${a[@]:1/0}` with no a, do not work out the offset, as in bash. `${x:1/0}` reports under the name as written, `x: 1/0: ...`.
- A report of arithmetic in `for`, `case` and a plain `while [ ]` gives their own line, not the line of the command before. A long expression is reported whole, and an assignment to a name over 255 characters works.
- `break` and `continue` in `$(...)` inside a loop end the substitution, quietly, and the loop goes on: `x=$(break; echo not)` is empty, as in bash. They used to warn and run the next command.
- `FUNCNAME`, `BASH_SOURCE`, `BASH_LINENO` and `caller` read one list, as in bash. A function names the file that defined it, wherever it is called from, a sourced file has an entry of its own, and the script is under them as `main`. `BASH_LINENO` is new. A bare `caller` at the top of a script prints `0 NULL` with status 0; `caller 0` prints the name as well as the line and file.
- `jobs | wc -l` lists the shell's jobs, on hosts too, and nothing else in a stage sees them. `jobs` takes `-l`, `-n`, `-p`, `-r`, `-s`, `-x` and job specs. It ignored them all, so `kill $(jobs -p)` could not work.
- `jobs` prints bash's line, with `+`, `-`, the padding and what the command was written as: `[1]+  Running                 sleep 9 | cat > f &`. It said `[1] Running cat`. A job that has ended is listed once, as `Done`, `Exit 3` or `Terminated`, and a prompt reports it. It was never reported anywhere.
- `%%`, `%+`, `%`, `%-`, `%name` and `%?text` work in `jobs`, `kill`, `wait`, `disown`, `fg` and `bg`, which knew only `%N`, and an ambiguous one says so. What has ended can still be named.
- A new job gets one past the highest number still known, where it took the lowest free one. `wait %1` can ask again until the next fork or `jobs`, as in bash.
- `fg` and `bg` need job control, at a prompt or under `set -m`, and say `no job control` anywhere else. `fg` prints the command it resumes, and `bg` says when a job is already in the background.
- `disown` takes `-a`, `-r` and `-h`, and says `no such job` in bash's words. `wait` and `kill` say where in the script their errors are, as other builtins do.
- At a prompt on a host, `[1] 1234` is printed for a job, as on iOS, and `( cmd ) &`, `{ cmd; } &`, a function and `a && b &` no longer stop for good on the terminal.
- `ulimit` is a builtin, on every platform. A file named `ulimit` on PATH could not change the shell, and Linux has none. It reads and sets `-c -d -f -l -m -n -p -s -t -u -v`, with `-S`, `-H` and `-a`. What a subshell, stage, job, substitution or nested shell sets ends with it.
- A script with no `#!` is read by exsh itself on a host, as bash reads it. It went to the system's `sh`, where a bashism failed and on Linux an array or `caller` was not there. A file with a NUL on its first line says `cannot execute binary file` and gives 126, where it was run as a script.
- On a plain build, `mkfifo p; echo x > p & cat p` no longer hangs the shell. exsh opened a job's redirections before it forked, and a FIFO's open waits for the other end, which was the shell's next command; at a prompt Ctrl-C could not free it. A job now opens its own redirections, each in its turn, as bash does. The same goes for `cat < p &`, functions, groups, subshells, loops and pipeline stages. Other forms change with it: `cat <&3 3< f` fails, `/bin/echo x > a <&9 > b` no longer creates `b`, `cat 3>&1 <<EOF` keeps its body, and `echo hi | cat < /dev/stdin` reads the pipe.
- A background command's or pipeline stage's words expand in it, as in bash. `: ${v:=1} &`, `echo $((v=5)) | cat` and `x=$(cmd) &` no longer change the shell, and `cmd $(slow) &` no longer holds the shell until `slow` ends. `echo a | echo $(cat)` reads the pipe. `$!`, `wait` and `kill` still reach the command.
- `exec cmd &` runs `cmd` as the job. It said `exec: cannot be used in background`.
- A background command or a pipeline stage leaves `$_` alone.
- A CHLD trap's own commands do not run it again. A pipeline, subshell or program in one kept that shell busy for ever.
- With job control off, a command stays in the shell's process group, as under bash. In a script on a terminal, `cat`, `stty`, `x=$(cat)` and `cat | tr` no longer stop for good, Ctrl-C reaches the command as well as the shell, and Ctrl-Z stops the script with its command, which `fg` resumes: the rest of the script used to be lost. `kill 0` reaches the whole script. `kill -- -$!` now says no such process, as bash does: a script that wants groups says `set -m`.
- Ctrl-C ends a script only if it ended the command, as in bash. A command that dealt with it leaves the script going, though a trap still runs, and `$?` is 130 when the command died of it. The EXIT trap runs before the script dies of Ctrl-C.
- A command dying of SIGINT, SIGQUIT or SIGTSTP with no job control says nothing about the shell. `sh -c 'kill -INT $$'; echo next` used to end the script.
- A script ignores SIGQUIT unless it traps it, as bash 5 does, and its command does not. `trap '' INT` and a signal ignored when the script started now reach its commands: they used to get the default.
- A job with no job control ignores Ctrl-C and Ctrl-\ for all its commands, not only the first, and a compound job whose stdin is the terminal reads `/dev/null`.
- `set -m` and `set +m` take effect: `$-`, `set -o`, whether a job gets a group of its own. They were accepted and ignored.
- `kill -TERM -- -123` works. It said `kill: invalid pid '--'`. `kill %1` reaches every stage of a job that has no group of its own, and `kill -N` signals the group, not only the commands of the job with that number.
- At a prompt, `( cmd ) &` no longer stops itself by taking the terminal.
- Ctrl-C ends a `read` waiting for a line, on hosts. It waited for the line, and a trap with it.

## Fixed in the iOS app
- `env test a == a`, and `xargs` or `find -exec` running `test`, know `==`, and take a value of `!`. They said `test: unsupported expression`.
- `/dev/stdout`, `/dev/stderr`, `/dev/stdin`, `/dev/fd/N` and `/proc/<pid>/fd/N` are the command's own descriptors, as on Linux, for the shell, applets and every frontend's file builtins, redirected and in a pipeline. `echo x > /dev/stdout` and a program writing to that name used to create a regular file under the app's root, so the output was lost with no error, and `cat /dev/stdin` said no such file. Opening one opens the file again, with its own offset, so `> /dev/stdout` truncates a redirected file and `cat /dev/stdin` reads it from the top. A pipe or the terminal gives a copy. `/proc/<pid>/fd/N` reaches that task's descriptor. `/dev` holds the links, and `ls -l /proc/self/fd` shows where each leads.
- A signal stays in the app, from any thread and whatever the pid. From a thread with no task of its own, `0`, `-1` and `-N` went to iOS's `kill`, which reads them as real process groups, and `-1` as every process the app may signal. A positive pid went there too with a signal such as SIGKILL, SIGHUP or SIGUSR1, so it reached whichever process had that number and not the task. Every pid now means the app's own tasks.
- The keyboard showing or hiding no longer types a space and a delete into the terminal. A running program got them as keys, for example when a graphics window took the keyboard away. The terminal scrolls back to the prompt instead.
- `eval` or `.` nested 16 or more deep no longer loses the shell's directory. From then on a relative path stopped meaning the directory `pwd` printed, so `cat`, `rm` and `>` found or made files somewhere else, and `cd` did not bring it back.
- `who am i` at a prompt prints the user, `pts/N` and the login time. Each terminal, a shell tab or an SSH tab, has a login in `/var/run/utmp` from before its startup file runs until its shell ends or its tab closes, and `/var/log/wtmp` gets the login and the logout. So `who`, `users` and `uptime` count open tabs, and `who -u` gives each tab's shell.
- A launch of the app is its boot. `who -b` and `who -r` answer, and `/var/run/utmp` starts empty, so a run iOS ended leaves no logins behind. `/var/log/wtmp` moves to `/var/log/wtmp.1` at a launch once it reaches 1 MB.
- `lps` no longer keeps an `exited` row for a task whose parent ended first. It stayed until the app restarted: `timeout 1 pascal x.pas` left one, and so did `( sleep 1 & )` and `sh -c 'sleep 1 &'`. A task like that shows as `running` while it runs, and its row goes when it ends, as init reaps one on Linux. Its end no longer runs the CHLD trap of the shell that took it in.
- `mkfifo` is a command, and a FIFO goes where the task's directory says, with the task's mask. `tar` and `cp` making a FIFO, and `mknod`, went to the host's directory.
- A file run with `.` at a prompt no longer announces its background jobs, as under bash.
- `ulimit` keeps each task's limits in the VM, and the open files limit holds its descriptors: a new task may have 1024 of them. Nothing else is enforced yet.
- `sftp`'s `lumask`, and `ssh` and `scp -p`, set the task's file mask, not the whole app's.
- What a command makes obeys its own file mask by every route, looser or tighter than the first tab's shell's. `git`, `ssh-keygen`, `ssh`, `scp`, `sftp` and `rsync` made files and directories through libc, which goes by one mask for the whole app, the first tab's shell's: `( umask 077; git init r )` left `r/.git` readable by everyone, and nothing `sftp` made after `lumask` obeyed it. So did a program's own `mkdir`, `creat` or `fopen`.
- `ssh` replacing a `known_hosts` entry and `ssh-keygen` saving a key no longer set the whole app's mask while they write, where a command in another tab could make its file with `077`.
- In a shell run as a command, `caller`, `FUNCNAME` and `BASH_SOURCE` show that script. They showed its caller's as well, since it runs on the caller's thread.
- `sh -c 'jobs | wc -l'` lists that shell's jobs, and in `$(...)` a stage lists the substitution's, not the top shell's.
- A program's stdout, stderr and stdin close when it ends. Each `pascal x > f` left one descriptor open for as long as the app ran, the file stayed open too, and after about 230 of them `pascal x | cat` failed with `Too many open files`.
- A program killed while it reads single keys, after `ReadKey` or `KeyPressed` in it or in a thread it spawned, puts the terminal back in line mode with echo. A background one left the terminal with no echo and no line editing.
- A program that reads single keys puts the terminal back as it found it. Each one put back the settings the first such program in the app had found, on whichever tab, so it undid an `stty` made since: at a `ReadLn` after the keys, and when it was killed.
- A thread that polled more than 128 descriptors no longer leaves about 5 KB behind when it ends.
- A killed program whose output goes to a pipe or FIFO that has stopped reading ends at once and lets go of it. `wait` reported it gone, but it stayed in its write, holding the FIFO open, until the reader moved. On the host test build of exsh, the reader then leaving ended the whole shell with status 141; the app ignores SIGPIPE and could not.
- A program started in a subshell has its own `$$`. It had the shell's, so `( sh -c 'kill -USR1 $$' )` ended the shell.
- `$RANDOM` in a job or a stage no longer moves the shell's sequence.
- `cat <(cmd) &` works. The file standing in for `<(cmd)` was gone before the job read it.
- With stdin closed (`cmd <&-`, or after `exec <&-`), `awk`, `wc`, `base64`, `gzip -c` and `xargs` fail and say why, as with the Mac's or Linux's tools. `awk` gave 0, `wc -l` and `base64` failed without a word, `gzip -c` wrote its header to stdout before it failed, and `xargs` gave 0. `[ -t 0 ]` and `tty` no longer take the session's terminal for a closed stdin: `exec <&-; [ -t 0 ]` said it was a terminal, and `tty <&-` or `tty < /dev/null` printed its name.
- A program's imports follow `cd` and take the app's absolute paths. After `cd d`, `clike ../p.cl` with `import "m.cl"` compiled the `m.cl` in the directory the app started in, with no error, and an import of `/home/x/m.cl` said `Could not open import`. This covered CLike's `import` and `#include`, Rea's `#import`, Aether's `use`, and Pascal's units under a relative or absolute `PASCAL_LIB_DIR`.
- A group, subshell, `if`, loop or function as a pipeline's first stage goes on when the stage after it stops reading. In `{ cat big; echo "x $?" > s; } | head -1` only `cat` ends, with status 141, as under bash, and the `echo` runs; the whole stage used to end there. A builtin writing to that pipe still ends the stage, as bash's subshell dies of SIGPIPE.
- A glob that ends in a slash takes only directories and keeps the slash, so `for d in */` no longer goes through every file as well. `echo */` printed files and directories alike, with no slash.
- `aether --version` names the language the app ships and the build: `Aether Compiler Version: 2026-10-30-1+<commit> (1.0, latest tag: ...)`. It said `1.01-dev`, the standalone repository's label for the release after 1.0, which does not exist yet. The app reports the nearest Aether release whose language is still its own, and `aether_version()` returns the same text.
- `~/Docs/Aether` holds the full Aether guide, `aether_for_llms_and_others.md`, ending in the inventory of every builtin, which is the app's own. `~/Docs` had the PSCAL documents only.
- Aether in the app has only its own builtins, as on a Mac or Linux. The shell's commands and its tools were Aether builtins too: `cat("f")` and `basename("/a/b")` compiled and ran, where they are `[SCOPE-001] identifier 'cat' not in scope`, and `builtins_json` listed about 180 of them, `[` and `__shell_exec` among them, with Aether's pure `sum` marked effectful after smallclue's `sum`. Aether's `printf` and `exec` ran the shell's commands. In any SDL build, `landscapedrawskydome` and `landscapedrawcloudlayer` are graphics builtins, and `pollkeyany` stays a core one.
- A program at the end of a pipeline, such as `echo | pascal prog`, killed from another task no longer leaves its tab waiting for ever. The thread that waits for the program's thread was killed with it, and the shell waited for that one.
- A subshell killed by its own job while in `wait`, as in `( p=$BASHPID; kill -TERM $p & wait $! )`, no longer ends the app. A cancel that came as a wait began unlocked the task table for the wrong thread, and the next wait found it corrupt.
- Killing the first stage of a pipeline from another task, as `sleep 3 | cat` with the `sleep` killed, no longer leaves the tab waiting for good. The killed stage kept its end of the pipe, so the stage reading it never saw the end.
- Ctrl-C ends an `rsync` to or from another host, with its `ssh`, and the prompt comes back with status 130. It waited out the transfer. A local `rsync` stops too, where its copy went on writing behind the prompt, and either way no temporary file is left.
- An `ssh` in the background keeps running when Ctrl-C is meant for the command in the foreground. It took the Ctrl-C and ended, and the foreground command did not.
- Two `ssh`, `scp` or `sftp` at once keep their own working state: how `sftp` reads a command and expands a remote glob, and the buffers behind their messages, times and option parsing.
- Each tab has an environment of its own, as each terminal's shell does on Linux: `export x=1` in one tab no longer shows in another, and what a command starts sees only what its shell exported. `ssh`, `scp`, `sftp` and `rsync` read that one too, `HOME`, `SSH_AUTH_SOCK` and a variable for `SendEnv` among them.
- At the keyboard, a program looping on `eof(stdin)` and `readln` gets every typed line and ends on Ctrl-D. It lost what was typed and never ended, and `read(a); read(b); readln(s)` got wrong values the same way. After a program waiting in `eof` was killed, the next one got nothing typed.
- Ctrl-C ends a VM program with status 130 and says nothing. On a Pascal `readln` it printed `--- VM execution Failed (Runtime Error) ---` and a debug line, and during `eof(stdin)` it gave 1.
- Ctrl-C ends `delay()` at once. The program ran on to the end of the delay.
- Ctrl-C ends the whole job, as it does on Linux: `wc` and `sort` print nothing, and `cat | tr a b` gives 130 and stops the rest of the line. `wc` printed `0 0 0`, and `cat | tr` gave 0 with the line running on.
- `/dev/tty` is the terminal of the tab a command runs in. The shell's own `> /dev/tty` and `< /dev/tty` opened a device of that name outside the app, which no tab draws, so `echo x > /dev/tty` showed nothing and `read x < /dev/tty` or `cat < /dev/tty` got nothing. A program's own open of it went by a table that a second tab could take over, so it could reach that tab's terminal, or a closed one's.
- In `ssh`, `scp`, `sftp` and `ssh-keygen`, `/dev/tty` is the terminal, not a copy of stdin, and a password or passphrase question goes there, as on Linux. A report of such a question going to the file with stderr redirected was not reproduced in this build: `ssh-keygen -y -f key 2> f` asks on the screen here, as it did in the build before this fix.
- `halt`, `reboot` or `poweroff` run from a job, and a signal such as `kill -HUP` that ends a shell waiting at its prompt, end it at once. It waited for the next line to be typed.
- A program's own fatal signal doesn't return: `kill(getpid(), SIGTERM)`, `raise`, and `sigprocmask` or `sigsuspend` letting in one that was waiting. They used to mark the task dead while it ran on. On a thread of its own it ends there. A command on the shell's or a subshell's thread leaves by a way back to the shell, which goes on with 128 + the signal. A simulated fork's child ends and its parent goes on from `fork`.
- `tail -f f | head -1` gives 141 for `tail`, not 1. What the shell starts takes SIGPIPE's default action, as a child does on a host, unless the shell ignores it with `trap '' PIPE`, and `kill -PIPE` ends a subshell.
- A stage that dies of a signal on a thread of its own shows 128 + the signal in `PIPESTATUS`, where it showed 0.
- A signal to a task that has ended but not been waited for succeeds and does nothing, for a pid, a group and `-1`, as on Linux. It said `No such process`, while `kill -0` said it was there. Ctrl-C and Ctrl-Z still find no one to take it in that group, and fall back as they did.
- A stage killed on a subshell's thread leaves that subshell going. `( a | b; echo x )` lost the `x` when `b` was killed.
- A stage killed on the shell's or a subshell's thread stops, and `$?` is 143 where it was 0. A call that waits gives up, an applet leaves at its next look for signals and a frontend the next time it calls or loops.
- A shell run as a command runs its EXIT trap: `sh -c`, `sh file` and a `#!` script. It never ran on iOS.
- A shell dying of a signal it sent itself runs its EXIT trap first, as bash does, except for KILL and QUIT, and `exit` in that trap doesn't change the status.

## Updated in the iOS app
- `ssh`, `scp`, `sftp` and `ssh-keygen` are OpenSSH 10.6p2, the latest upstream release (2026-11-05), up from 10.2p1. `ssh -V` says so. Every change the app makes to OpenSSH is carried over. Security fixes they gain:
  - sftp checks the paths a server returns in a recursive download, which could write outside the target directory (10.6), and `sftp host:/path .` can't be sent to another local path (10.4).
  - scp between two remote hosts can't be made to write into the parent of the target (10.4), and `scp -O` clears setuid and setgid bits on what it downloads (10.3).
  - ssh no longer uses freed memory when a server changes its host key during a rekey (10.4), or when a forwarding is added through a multiplexing master while another is pending (10.5).
  - Compression no longer shares its dictionary across channels, which let one channel's data be recovered through another's (10.6). Compressed data can't grow past the largest packet (10.6).
  - A user name given on the command line is checked before `%u` can reach `Match exec` or `ProxyCommand`. `$` and `\` in one are refused (10.3, 10.6).
  - Key types are checked against the allowed lists before a key is parsed, key files with an absurd KDF round count are refused, and ssh-keygen gets certificate expiry right across a daylight saving change (10.6).
  - One-byte out-of-bounds reads in sftp are fixed, and so is a download loop on a server that keeps returning nothing (10.3, 10.4).
  - Not in the app: 10.6p2's own security fix (sshd on musl Linux without PAM), and the sshd and ssh-agent fixes.
- Changes you may notice: a user name with `$` or `\` on the command line is refused (a `User` line in a config file still works), `Compression yes` compresses less, and `scp -R` warns that it is deprecated. A peer that can't rekey now fails when a rekey comes due, as does one that sends other messages during a rekey. The post-quantum key type `ssh-mldsa44-ed25519` is accepted by default (`ssh-keygen -t mldsa44-ed25519`), and keys made by 10.3's experimental form of it must be made again. ssh-keygen encrypts new keys with 32 KDF rounds instead of 24. No `ssh_config` keyword was added or removed.

## Fixed in the frontends on every platform
- A bytecode cache entry covers the files its program imports. An import is also looked for in the working directory, so one program can compile to two from two directories, and run from the second it could reuse the first's entry. Rea and Aether could also miss an edit to a module that an imported module uses.
- The Rea `landscape` demo runs on macOS. It crashed in `glCreateShader` at its first frame: a 3D window there draws with the SDL renderer and has no OpenGL context, and `LandscapeDrawCloudLayer` called OpenGL anyway. The demo now draws its sprite clouds there. `LandscapeDrawCloudLayer` and `LandscapeDrawSkyDome` without a current OpenGL context are a runtime error, and the new `LandscapeShadersAvailable()` says whether they can draw.
- 3D programs on macOS and iOS no longer show a black window when part of a scene lies behind the camera. Such a shape made the frame fall back to the software buffer, which was then shown in place of everything drawn with Metal. The Rea `landscape` demo showed only black.
- 3D programs on macOS and iOS no longer crash or lose parts of a scene when a frame draws a lot. Each shape's points were first written over the start of the frame's earlier shapes, and when the frame's store had to grow, it was read from where it had been: the Rea `landscape` demo crashed after a while and showed shapes missing.
- A script's commands share its process group, so Ctrl-C and Ctrl-Z reach them. Ctrl-C ends a script run with `sh`, and its `trap ... INT` and EXIT trap run, where SIGINT used to wait unseen on a nested shell. A command the shell starts no longer inherits the shell's traps as handlers, so a signal ends it.
- `sleep` and other applets that poll on the shell's own thread end when a signal kills them.
- A fatal signal a shell sends itself ends it: `( kill -TERM $BASHPID; echo x )` printed x, after its parent had been told it was gone.
- `sh file` and `./file` at a prompt stop on Ctrl-Z and go on with `fg`, with their commands. `sh file` used to finish its command and then end, and `./file` kept the prompt's job control: `$-` had `im`, and a job had a group of its own.

## Faster in exsh
- Commands that stay in the shell take about 0.67 of the time they did, which is about 1.21x bash 5 where it was 1.69x. The numbers are from `tools/shellbench_cacheable`, 3 rounds on one Mac, so they differ from the laptop's in V.1R32's notes. Commands that start a process are unchanged.
- A function calling a function takes 2.6 µs, down from 11.1 µs, and three deep 3.4 µs, down from 24.9 µs. Functions, `eval`, `source` and traps take a VM from a pool of 16, where only the first level kept one.
- `:` takes 0.64 µs, down from 0.97 µs, an assignment 0.95 µs, down from 1.46 µs, and `eval` 4.7 µs, down from 6.0 µs. A command reads its metadata in place instead of copying it, sets `EXSH_LAST_STATUS` only when it changes, and reads debug switches such as `PSCALI_REDIR_DEBUG` again only after the shell sets a variable.
- `[ ]` among 10,000 files took twice as long as in an empty directory, and now takes the same.

- `<(cmd)` and `>(cmd)` are a pipe with a command running beside whatever names it, as on Linux, and the word is `/dev/fd/N`. `exec 3> >(cmd)` and `exec > >(tee log)` lost everything written afterwards, because a file stood in and the command read it at once. `head -1 <(yes)` never ended and filled the disk. `tee >(a) >(b)` ran its commands one after the other, once `tee` was done.

## Differs from bash
- A job's text is as it was written, where bash prints a compound command back on several lines in its own layout. `jobs -l` gives a pipeline one line, where bash gives each stage one. The padding is bash's on Linux; bash 5.3 on macOS pads further.
- `jobs` right after `kill %1` says the job is terminated, where bash, which has not yet heard, says `Running`. At a prompt a job that ends during a line waits for the prompt to be reported, where bash reports it when the next command ends. `-` on a job that had already ended when the next one started can differ, since bash decides that by timing. At a prompt, `fg` still clears the screen and says `continued`.
- `jobs -x` puts in a job's first pid, where bash in a script puts in the shell's group. In a script without `set -m`, a stopped job is listed as `Stopped`, where bash does not notice and says `Running`.
- A hard limit set inside `$(...)` on a host sets only the soft one, as the substitution runs in the shell's process and a hard limit could not come back up.

- Unquoted parentheses among a command's words are plain words: `find . ( -name a )` runs, and `f(x) { :; }` reports `}` where bash reports `x`. The owner chose to keep this.
- A `[[ ]]` bash can't parse, such as `[[ a == ]]`, runs.
- Text that runs out says what 3.2 and 5.2 say. 5.3 adds which construct was open, and where.
- Arithmetic syntax errors say `syntax error`, as bash does up to 5.2, where 5.3 says `arithmetic syntax error`. `[ a -eq 1 ]` says `integer expression expected`, where 5.3 says `integer expected`.
- After failed arithmetic in a compound command over several lines, what follows it on its last line runs, where bash drops it: `{ echo $((1/0))` and then `}; echo x`.
- `y=1 z=$((1/0))` assigns nothing, where bash has set y by then. `cat <<< $((1/0)); echo x` drops `echo x`, where bash runs it. An integer variable's assignment as a pipeline's stage, `n=1/0 | cat`, says nothing.

## Differs from Linux on iOS
- A script at top level that writes to a pipe whose reader has gone, such as `exec 3> >(head -1)`, gets a write error and carries on. On Linux SIGPIPE ends it with status 141. A subshell does end.
- Opening `/dev/stdin` or `/dev/fd/N` on a pipe in the direction the descriptor doesn't go says `Permission denied`, where Linux opens the pipe's other end. On a socket it gives a copy, where Linux says `No such device or address`.
- A symlink to `/dev/stdout` reaches the app's own stdout and not the command's: after `ln -s /dev/stdout x`, `echo a > x` is lost. Any symlink whose target starts with `/` is followed outside the app's root.
- `ls -ld /dev/fd` shows a directory, though `readlink /dev/fd` answers `/proc/self/fd`.
- Libraries that open files themselves, such as sqlite's, and the Go tools don't see these names.
- After an upgrade, a `/dev/stdout` file left by an earlier build stays until removed. The name still reaches the descriptor, but `ls` and `[ -L ]` see the file.
- `who -u` gives `?` as a terminal's idle time, because a terminal in the app keeps no access time.
- `echo */.` lists files as well as directories and drops the `/.`, because resolving the pattern against the task's directory drops it.
- Killing a shell takes the command it is running with it: `sh -c 'pascal x.pas; echo y' &` killed ends `pascal` too, where on Linux it runs on as an orphan. `sh -c 'pascal x.pas'` and `( pascal x.pas )` end as they do on Linux, where the shell becomes the command.
- `$BASHPID` in `$(...)` is the shell's: a substitution has no task of its own. So `x=$(kill -TERM $BASHPID)` ends the shell, where bash ends only the substitution.
- A subshell with an EXIT trap killed from another task doesn't run it, because its thread is cancelled. Bash runs it.
- A shell whose thread can't be cancelled acts on a fatal signal from another task only when its current command is done, where Linux ends it at once and leaves the command running.
- A command on the shell's or a subshell's thread that doesn't look for signals keeps running when killed from another task, where Linux ends it. A call that waits gives up, and the rest runs until the command ends.
- A fatal signal from code with no way back to the shell, on a thread that has to go on, returns with the task marked as ending. Only applets have that way back, and a frontend stops the next time it calls or loops.

## Known Issues
- `micro` makes a new file with the first tab's shell's mask, not its own: the Go runtime reaches libc directly.
Descriptor names, on iOS:
- `/proc/$$/fd/1` inside a redirected group misses the redirection: `{ echo a > /proc/$$/fd/1; } > f` leaves `f` empty. Host builds write `a` to `f`, as Linux does. Seen through the smoke tool, which runs in the nested `.exshrc` context, not at a prompt.
- `readlink /proc/self/fd/3` on a file, and `readlink -f /dev/stdout` with stdout on a file, don't give the path that `$HOME` names it by. Host builds do.

Pipes, on iOS:
- A program writing to a pipe or FIFO whose reader has gone, as in `clike x | head -1`, runs on, and the line never ends: its writes fail with `EPIPE`, where on Linux SIGPIPE ends it. `kill` ends it.

Terminals, on iOS:
- Ctrl-C in a script whose background job has already ended reaches the tab's own shell, not the script: the command ends and the script goes on. `sleep 1 & sleep 30; echo "no $?"` run with `sh` prints `no 130`, where bash ends the script.
- The terminal's size can be wrong after an SDL program exits, and `reset` doesn't restore it. Seen on an iPad, not reproduced in the simulator.
