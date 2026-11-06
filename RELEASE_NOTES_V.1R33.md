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
- In a shell run as a command, `caller`, `FUNCNAME` and `BASH_SOURCE` show that script. They showed its caller's as well, since it runs on the caller's thread.
- `sh -c 'jobs | wc -l'` lists that shell's jobs, and in `$(...)` a stage lists the substitution's, not the top shell's.
- A program's stdout, stderr and stdin close when it ends. Each `pascal x > f` left one descriptor open for as long as the app ran, the file stayed open too, and after about 230 of them `pascal x | cat` failed with `Too many open files`.
- A program killed while it reads single keys, after `ReadKey` or `KeyPressed` in it or in a thread it spawned, puts the terminal back in line mode with echo. A background one left the terminal with no echo and no line editing.
- A thread that polled more than 128 descriptors no longer leaves about 5 KB behind when it ends.


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
- Opening `/dev/stdin` or `/dev/fd/N` on a pipe in the direction the descriptor doesn't go says `Permission denied`, where Linux opens the pipe's other end. On a socket it gives a copy, where Linux says `No such device or address`.
- A symlink to `/dev/stdout` reaches the app's own stdout and not the command's: after `ln -s /dev/stdout x`, `echo a > x` is lost. Any symlink whose target starts with `/` is followed outside the app's root.
- `ls -ld /dev/fd` shows a directory, though `readlink /dev/fd` answers `/proc/self/fd`.
- Libraries that open files themselves, such as sqlite's, and the Go tools don't see these names.
- After an upgrade, a `/dev/stdout` file left by an earlier build stays until removed. The name still reaches the descriptor, but `ls` and `[ -L ]` see the file.
- `who -u` gives `?` as a terminal's idle time, because a terminal in the app keeps no access time.
- Killing a shell takes the command it is running with it: `sh -c 'pascal x.pas; echo y' &` killed ends `pascal` too, where on Linux it runs on as an orphan. `sh -c 'pascal x.pas'` and `( pascal x.pas )` end as they do on Linux, where the shell becomes the command.

## Known Issues
Descriptor names, on iOS:
- `/proc/$$/fd/1` inside a redirected group misses the redirection: `{ echo a > /proc/$$/fd/1; } > f` leaves `f` empty. Host builds write `a` to `f`, as Linux does. Seen through the smoke tool, which runs in the nested `.exshrc` context, not at a prompt.
- `readlink /proc/self/fd/3` on a file, and `readlink -f /dev/stdout` with stdout on a file, don't give the path that `$HOME` names it by. Host builds do.

Terminals, on iOS:
- Writing to `/dev/tty` shows nothing on screen, though the write succeeds, as `echo x > /dev/tty`.
- `/dev/tty` fails in the first tab once a second tab has opened and closed, and in a second tab while it is open. `stty -F /dev/tty` says so.
