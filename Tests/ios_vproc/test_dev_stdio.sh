#!/bin/bash
# /dev/stdin, /dev/stdout, /dev/stderr, /dev/fd/N and /proc/self/fd/N act as on Linux, for the shell and
# for a VM program, redirected and in a pipeline. The shell's cases give the same results under Linux's
# bash. These used to be plain paths under the app's root, so output went into <root>/dev/stdout with no
# error and `cat /dev/stdin` said no such file. Each case runs in its own throwaway root, and none may
# leave a regular file of those names in its dev directory.
#   test_dev_stdio.sh <exsh>
set -u
EXE=${1:?usage: test_dev_stdio.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-dev-stdio.XXXXXX") || exit 1
case "$BASE" in
    */exsh-dev-stdio.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

SRC="$BASE/src"
mkdir -p "$SRC"
printf 'b\na\nc\n' > "$SRC/d.txt"
printf 'echo from script\n' > "$SRC/s.sh"

cat > "$SRC/o.pas" <<'EOF'
program O;
var f: text;
begin
  assign(f, '/dev/stdout'); rewrite(f); writeln(f, 'pascal out ', 6*7); close(f);
end.
EOF
cat > "$SRC/e.pas" <<'EOF'
program E;
var f: text;
begin
  assign(f, '/dev/stderr'); rewrite(f); writeln(f, 'pascal err'); close(f);
end.
EOF
cat > "$SRC/i.pas" <<'EOF'
program I;
var f: text; s: string;
begin
  assign(f, '/dev/stdin'); reset(f);
  while not eof(f) do begin readln(f, s); writeln('got ', s); end;
  close(f);
end.
EOF
# Halt ends the program with its file open: nothing is there to flush it for it.
cat > "$SRC/f.pas" <<'EOF'
program F;
var f: text;
begin
  assign(f, '/dev/fd/3'); rewrite(f); writeln(f, 'pascal fd'); close(f);
end.
EOF
cat > "$SRC/q.pas" <<'EOF'
program Q;
begin
  writeln(fileexists('/dev/stdout'), ' ', fileexists('/dev/stdin'));
end.
EOF
cat > "$SRC/o.clike" <<'EOF'
int main() {
    text f;
    assign(f, "/dev/stdout"); rewrite(f); fprintf(f, "clike out %d\n", 42); close(f);
    return 0;
}
EOF
cat > "$SRC/e.clike" <<'EOF'
int main() {
    text f;
    assign(f, "/dev/stderr"); rewrite(f); fprintf(f, "clike err\n"); close(f);
    return 0;
}
EOF
cat > "$SRC/i.clike" <<'EOF'
int main() {
    text f;
    str s;
    assign(f, "/dev/stdin"); reset(f); readln(f, s); printf("got %s\n", s); close(f);
    return 0;
}
EOF
cat > "$SRC/o.rea" <<'EOF'
class Main {}
text f;
assign(f, "/dev/stdout"); rewrite(f); writeln(f, "rea out"); close(f);
EOF
cat > "$SRC/e.rea" <<'EOF'
class Main {}
text f;
assign(f, "/dev/stderr"); rewrite(f); writeln(f, "rea err"); close(f);
EOF
cat > "$SRC/i.rea" <<'EOF'
class Main {}
text f;
str s;
assign(f, "/dev/stdin"); reset(f); readln(f, s); writeln("got ", s); close(f);
EOF
cat > "$SRC/o.aether" <<'EOF'
fn main() -> Void {
    fx {
        write_file("/dev/stdout", "aether out\n");
        append_file("/dev/stdout", "aether more\n");
    }
    ret;
}
EOF
cat > "$SRC/e.aether" <<'EOF'
fn main() -> Void {
    fx {
        write_file("/dev/stderr", "aether err\n");
    }
    ret;
}
EOF
cat > "$SRC/i.aether" <<'EOF'
fn main() -> Void {
    fx {
        let lines: Text[] = read_lines("/dev/stdin");
        println("got ", lines[0]);
    }
    ret;
}
EOF

fails=0
n=0
hangs=0

# check <name> <script> <output>, with the script's stdout going to a file, stderr to the same one
# and stdin at /dev/null. Stops a case that hangs after 20 s, and the whole test after five of them.
check() {
    local name=$1 script=$2 want=$3
    n=$((n + 1))
    local T="$BASE/r$n" out rc f problem=""
    mkdir -p "$T"
    cp "$SRC"/* "$T"/
    printf '%s\n' "$script" > "$T/t.sh"
    (cd "$T" && PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" /usr/bin/perl -e 'alarm 20; exec @ARGV' "$EXE" --no-cache t.sh > out.txt 2>&1 < /dev/null)
    rc=$?
    if [ "$rc" = 142 ]; then
        hangs=$((hangs + 1))
        if [ "$hangs" -ge 5 ]; then
            echo "FAIL $name: giving up after $hangs hangs"
            exit 1
        fi
    fi
    out=$(cat "$T/out.txt")
    [ "$rc" = 0 ] || problem="status $rc"
    [ "$out" = "$want" ] || problem="${problem:+$problem, }wrong output"
    for f in stdin stdout stderr; do
        if [ -e "$T/dev/$f" ] && [ ! -L "$T/dev/$f" ]; then
            problem="${problem:+$problem, }dev/$f was left in the root"
        fi
    done
    if [ -n "$problem" ]; then
        echo "FAIL $name: $problem"
        printf 'want:\n%s\ngot:\n%s\n' "$want" "$out"
        fails=$((fails + 1))
    else
        echo "ok $name"
    fi
}

# The shell's own redirections.
check stdout_redirect 'echo a > /dev/stdout' 'a'
check stderr_redirect 'echo a > /dev/stderr' 'a'
check stdout_truncates '{ echo a > /dev/stdout; echo b > /dev/stdout; } > f; cat f' 'b'
check stdout_appends '{ echo a >> /dev/stdout; echo b >> /dev/stdout; } > f; cat f' 'a
b'
check stdout_pipe_twice '{ echo a > /dev/stdout; echo b > /dev/stdout; } | cat' 'a
b'
check stderr_in_group '{ echo a > /dev/stderr; } 2> f; echo got; cat f' 'got
a'
check stdout_stays_when_stderr_moves 'echo a 2> /dev/stdout > f; cat f' 'a'
check stdout_in_pipeline 'echo a > /dev/stdout | tr a b' 'b'
check stdout_in_subshell '( echo a > /dev/stdout ) > f; cat f' 'a'
check stdout_append 'echo a >> /dev/stdout' 'a'
check stdout_both_streams '{ echo a &> /dev/stdout; } > f; cat f' 'a'
check stdout_exec_fd 'exec 3> /dev/stdout; echo a >&3; exec 3>&-' 'a'
check stdout_exec_whole 'exec > /dev/stderr; echo a' 'a'
check stdout_in_job '{ echo a > /dev/stdout & wait; } > f; cat f' 'a'
check stdout_in_function 'f() { echo a > /dev/stdout; }; f > g; cat g' 'a'
check stdin_redirect '{ cat < /dev/stdin; } < d.txt' 'b
a
c'
check stdin_read '{ read x < /dev/stdin; echo "[$x]"; } < d.txt' '[b]'
check stdin_exec_fd '{ exec 3< /dev/stdin; cat <&3; exec 3<&-; } < d.txt' 'b
a
c'
# Its own offset, from the top, where a duplicate shares the shell's.
check stdin_from_the_top '{ read x; cat /dev/stdin; } < d.txt' 'b
a
c'
check stdin_dup_shares '{ read x; cat <&0; } < d.txt' 'a
c'
# How the descriptor was opened does not matter: the file is opened again.
check stdin_written_through 'cp d.txt e; { echo a > /dev/stdin; } < e; cat e' 'a'
check stdout_read_through 'cat /dev/stdout > f 2>&1; cat f' ''
# Spelled another way, or from the directory: one name for the same thing.
check spelled_with_slashes 'echo a > //dev//stdout' 'a'
check spelled_with_dots 'echo a > /dev/../dev/./stdout' 'a'
check spelled_from_dev_dir 'cd /dev; echo a > stdout; cd /' 'a'
check other_stdout_is_a_file 'mkdir dev2; echo a > dev2/stdout; cat dev2/stdout; echo b > stdout; cat stdout' 'a
b'

# A closed descriptor's link leads nowhere.
check stdout_closed '( exec >&-; echo a > /dev/stdout ) 2>&1 | sed "s/^.*: //"' 'No such file or directory'
check stdin_closed '( exec <&-; cat /dev/stdin ) 2>&1 | sed "s/^.*: //"' 'No such file or directory'
check noclobber_on_a_pipe 'set -C; echo a > /dev/stdout | cat; echo "rc $?"' 'a
rc 0'

# Test operators and path calls.
check test_exists '[ -e /dev/stdout ] && echo e; [ -e /dev/stderr ] && echo e; [ -e /dev/stdin ] && echo e' 'e
e
e'
check test_access '[ -w /dev/stdout ] && echo w; [ -r /dev/stdin ] && echo r; [ -x /dev/stdout ] || echo no-x' 'w
r
no-x'
check test_kind 'echo x | { [ -p /dev/stdin ] && echo pipe; }; { [ -f /dev/stdin ] && echo file; } < d.txt' 'pipe
file'
check test_closed '( exec <&-; [ -e /dev/stdin ] && echo there || echo gone )' 'gone'
check rm_keeps_it 'rm -f /dev/stdout 2> /dev/null; echo a > /dev/stdout' 'a'
check links '[ -L /dev/stdout ] && echo link; readlink /dev/stdout; readlink /dev/stderr; readlink /dev/stdin; readlink /dev/fd' 'link
/proc/self/fd/1
/proc/self/fd/2
/proc/self/fd/0
/proc/self/fd'
check dev_listing 'ls /dev | grep -E "^(fd|stderr|stdin|stdout)$"' 'fd
stderr
stdin
stdout'
check ls_shows_link 'ls -l /dev/stdout | sed "s/.*\/dev/\/dev/"' '/dev/stdout -> /proc/self/fd/1'
check realpath_of_file 'mkdir w; cd w; readlink -f /dev/stdout > y; [ "$(cat y)" = "$PWD/y" ] && echo same > z; cat z' 'same'
check realpath_of_pipe 'readlink -f /dev/stdout | sed "s/[0-9][0-9]*/N/g"' '/proc/N/fd/pipe:[N]'

# /dev/fd/N and /proc/self/fd/N.
check fd_read '{ cat /dev/fd/0; } < d.txt' 'b
a
c'
check fd_write 'echo a > /dev/fd/1' 'a'
check fd_stderr 'echo a > /dev/fd/2' 'a'
check fd_pipe 'echo hi | cat /dev/fd/0' 'hi'
check fd_high 'exec 3< d.txt; cat /dev/fd/3; exec 3<&-' 'b
a
c'
check fd_high_write 'exec 3> f; echo a > /dev/fd/3; exec 3>&-; cat f' 'a'
check fd_from_the_top 'exec 3< d.txt; read x <&3; cat /dev/fd/3; exec 3<&-' 'b
a
c'
check fd_truncates 'exec 3> f; echo one >&3; echo two > /dev/fd/3; exec 3>&-; cat f' 'two'
check fd_appends 'exec 3> f; echo one >&3; echo two >> /dev/fd/3; exec 3>&-; cat f' 'one
two'
check fd_missing 'cat /dev/fd/7 2>&1 | sed "s/^.*: //"' 'No such file or directory'
check fd_exists '[ -e /dev/fd/0 ] && echo y; [ -e /dev/fd/7 ] || echo n' 'y
n'
check fd_listing 'ls /dev/fd/ | grep -x 0; exec 3< d.txt; ls /dev/fd/ | grep -x 3; exec 3<&-' '0
3'
check fd_removed_file 'exec 3< d.txt; rm d.txt; cat /dev/fd/3; exec 3<&-' 'b
a
c'
check fd_renamed_file 'exec 3< d.txt; mv d.txt e.txt; cat /dev/fd/3; exec 3<&-' 'b
a
c'
check fd_name_taken_over 'exec 3< d.txt; mv d.txt e.txt; echo other > d.txt; cat /dev/fd/3; exec 3<&-' 'b
a
c'
check fd_write_name_taken_over 'exec 3> f; mv f g; echo other > f; echo a > /dev/fd/3; exec 3>&-; cat f g' 'other
a'
check fd_from_its_directory 'exec 3< d.txt; cd /dev/fd; cat 3; cd /; exec 3<&-' 'b
a
c'
check proc_read '{ cat /proc/self/fd/0; } < d.txt' 'b
a
c'
check proc_write 'echo a > /proc/self/fd/1' 'a'
check proc_own_pid 'echo a > /proc/$$/fd/1' 'a'
# A command has a vproc of its own, and $$ is the shell's. The name still means that process.
check proc_shell_redirected '{ echo a > /proc/$$/fd/1; } > f; cat f' 'a'
check proc_shell_from_a_stage '{ { echo a >> /proc/$$/fd/1; echo b; } | tr b c > g; } > f; cat f g' 'a
c'
check proc_other_task 'sleep 5 < d.txt & p=$!; sleep 0.3; cat /proc/$p/fd/0; [ -f /proc/$p/fd/0 ] && echo file; kill $p; wait $p 2>/dev/null; echo done' 'b
a
c
file
done'
check proc_other_closed 'sleep 5 3<&- & p=$!; sleep 0.3; [ -e /proc/$p/fd/3 ] || echo gone; kill $p; wait $p 2>/dev/null; echo done' 'gone
done'
check proc_no_such_task 'echo a 2>/dev/null > /proc/99999/fd/1; echo "rc $?"; [ -e /proc/99999/fd/1 ] || echo gone' 'rc 1
gone'
check proc_link 'mkdir w; cd w; echo x > y; exec 3< y; [ "$(readlink /proc/self/fd/3)" = "$PWD/y" ] && echo same; exec 3<&-' 'same'

# Applets, which read and write what the name gives them.
check cat_file 'cat /dev/stdin < d.txt' 'b
a
c'
check cat_pipe 'echo hi | cat /dev/stdin' 'hi'
check cat_twice 'cat /dev/stdin /dev/stdin < d.txt' 'b
a
c
b
a
c'
check head_file 'head -1 /dev/stdin < d.txt' 'b'
check tail_pipe 'cat d.txt | tail -1 /dev/stdin' 'c'
check wc_pipe 'cat d.txt | wc -l /dev/stdin | sed "s/ *\([0-9]*\).*/\1/"' '3'
check sort_pipe 'cat d.txt | sort /dev/stdin' 'a
b
c'
check grep_pipe 'cat d.txt | grep a /dev/stdin' 'a'
check sed_pipe 'cat d.txt | sed -n 2p /dev/stdin' 'a'
check awk_pipe 'cat d.txt | awk "NR==3" /dev/stdin' 'c'
check tee_stdout 'echo a | tee /dev/stdout | cat' 'a
a'
check tee_stderr '{ echo a | tee /dev/stderr; } 2>&1 | cat' 'a
a'
check tee_own_offset 'echo a | tee /dev/stdout' 'a'
check cp_to_stdout 'cp d.txt /dev/stdout' 'b
a
c'
check cp_from_stdin 'cp /dev/stdin f < d.txt; cat f' 'b
a
c'

# Nested shells and scripts, which run on a vproc of their own.
check sh_c 'sh -c "echo a > /dev/stdout" > f; cat f' 'a'
check sh_script_stdin 'sh /dev/stdin < s.sh' 'from script'
check sh_script_pipe 'cat s.sh | sh /dev/stdin' 'from script'
check sh_script_pipe_twice 'echo "echo one" | sh /dev/stdin; echo "echo two" | sh /dev/stdin' 'one
two'
check source_stdin '. /dev/stdin < s.sh' 'from script'

# Frontends: a redirection on the command wins, and a pipeline's stage gets its own.
check pascal_stdout 'pascal --no-cache o.pas' 'pascal out 42'
check pascal_stdout_redirected 'pascal --no-cache o.pas > f; echo got; cat f' 'got
pascal out 42'
check pascal_stdout_pipe 'pascal --no-cache o.pas | tr a-z A-Z' 'PASCAL OUT 42'
check pascal_stderr 'pascal --no-cache e.pas' 'pascal err'
check pascal_stderr_redirected 'pascal --no-cache e.pas 2> f; echo got; cat f' 'got
pascal err'
check pascal_stderr_not_stdout 'pascal --no-cache e.pas > f 2> g; echo "out [$(cat f)]"; cat g' 'out []
pascal err'
check pascal_stdin_file 'pascal --no-cache i.pas < d.txt' 'got b
got a
got c'
check pascal_stdin_pipe 'cat d.txt | pascal --no-cache i.pas' 'got b
got a
got c'
check pascal_stdin_from_the_top '{ read x; pascal --no-cache i.pas; } < d.txt' 'got b
got a
got c'
check pascal_exists 'pascal --no-cache q.pas' 'TRUE TRUE'
check pascal_fd_write 'pascal --no-cache f.pas 3> f; cat f' 'pascal fd'
check pascal_source_stdin 'pascal --no-cache /dev/stdin < o.pas' 'pascal out 42'
check clike_stdout 'clike --no-cache o.clike' 'clike out 42'
check clike_stdout_redirected 'clike --no-cache o.clike > f; echo got; cat f' 'got
clike out 42'
check clike_stdout_pipe 'clike --no-cache o.clike | tr a-z A-Z' 'CLIKE OUT 42'
check clike_stderr_redirected 'clike --no-cache e.clike 2> f; echo got; cat f' 'got
clike err'
check clike_stdin_file 'clike --no-cache i.clike < d.txt' 'got b'
check clike_stdin_pipe 'cat d.txt | clike --no-cache i.clike' 'got b'
check rea_stdout 'rea --no-cache o.rea' 'rea out'
check rea_stdout_redirected 'rea --no-cache o.rea > f; echo got; cat f' 'got
rea out'
check rea_stdout_pipe 'rea --no-cache o.rea | tr a-z A-Z' 'REA OUT'
check rea_stderr_redirected 'rea --no-cache e.rea 2> f; echo got; cat f' 'got
rea err'
check rea_stdin_file 'rea --no-cache i.rea < d.txt' 'got b'
check rea_stdin_pipe 'cat d.txt | rea --no-cache i.rea' 'got b'
check aether_stdout 'aether --no-cache o.aether' 'aether out
aether more'
check aether_stdout_redirected 'aether --no-cache o.aether > f; echo got; cat f' 'got
aether out
aether more'
check aether_stdout_pipe 'aether --no-cache o.aether | tr a-z A-Z' 'AETHER OUT
AETHER MORE'
check aether_stderr_redirected 'aether --no-cache e.aether 2> f; echo got; cat f' 'got
aether err'
check aether_stdin_file 'aether --no-cache i.aether < d.txt' 'got b'
check aether_stdin_pipe 'cat d.txt | aether --no-cache i.aether' 'got b'
check frontend_in_sh_c 'sh -c "pascal --no-cache o.pas" > f; echo got; cat f' 'got
pascal out 42'
check frontend_truncates '{ pascal --no-cache o.pas; clike --no-cache o.clike; } > f; cat f' 'clike out 42'
check frontend_pipe_twice '{ pascal --no-cache o.pas; clike --no-cache o.clike; } | cat' 'pascal out 42
clike out 42'
check frontend_job 'pascal --no-cache o.pas > f & wait; cat f' 'pascal out 42'

echo "test_dev_stdio: $n cases, $fails failed"
[ "$fails" = 0 ]
