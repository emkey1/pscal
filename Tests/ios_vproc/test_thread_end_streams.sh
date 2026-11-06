#!/bin/bash
# A thread's cached stdio streams close when the thread ends. Everything runs in one process here,
# so each frontend run with stdout on a host descriptor used to leave that descriptor open for the
# life of the app. The count comes from lsof outside the process: at a low limit a full table only
# shows now and then, because a stream that can't dup falls back. The rest check that a kill still
# ends cleanly with data in the buffer. Halt isn't here: exit() is the real one
# on a host, so it's a smoke row. Each case runs in its own throwaway root.
#   test_thread_end_streams.sh <exsh>
set -u
EXE=${1:?usage: test_thread_end_streams.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-thread-streams.XXXXXX") || exit 1
case "$BASE" in
    */exsh-thread-streams.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
READER=""
trap '[ -n "$READER" ] && kill "$READER" 2>/dev/null; rm -rf "$BASE"' EXIT

ROUNDS=60
ALLOWED=25
fails=0
n=0

setup() {
    n=$((n + 1))
    T="$BASE/r$n"
    mkdir -p "$T"
    printf "program H;\nbegin\n  writeln('hello');\nend.\n" > "$T/h.pas"
    printf 'int main() {\n    printf("hello\\n");\n    return 0;\n}\n' > "$T/h.clike"
    printf 'int main() {\n    str s;\n    readln(stdin, s);\n    printf("[%%s]\\n", s);\n    return 0;\n}\n' > "$T/r.clike"
    printf 'void w() {\n    printf("in thread\\n");\n}\nint main() {\n    int t = spawn w();\n    join t;\n    return 0;\n}\n' > "$T/t.clike"
    printf 'int main() {\n    while (1) {\n        printf("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\n");\n    }\n    return 0;\n}\n' > "$T/flood.clike"
    printf 'line\n' > "$T/in.txt"
}

# Stops a shell that hangs after 90 s. The caller's $! is the shell itself.
start() {
    ( cd "$T" && PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" exec /usr/bin/perl -e 'alarm 90; exec @ARGV' "$EXE" --no-cache t.sh > out.txt 2> err.txt < /dev/null ) &
    SHELL_PID=$!
}

# Waits for the script to reach the mark file, and says how many descriptors the shell has.
count_at() {
    local i=0
    while [ ! -e "$T/$1" ] && [ "$i" -lt 900 ] && kill -0 "$SHELL_PID" 2>/dev/null; do
        sleep 0.1
        i=$((i + 1))
    done
    lsof -p "$SHELL_PID" 2>/dev/null | wc -l | tr -d ' '
}

# flat <name> <one round>: the descriptor count after ROUNDS of a form against the one before.
flat() {
    local name=$1 round=$2 before after rc
    setup
    cat > "$T/t.sh" <<EOF
round() { $round
}
round
: > mark1
while [ ! -e go1 ]; do sleep 1; done
i=0
while [ \$i -lt $ROUNDS ]; do
    round
    i=\$((i + 1))
done
: > mark2
while [ ! -e go2 ]; do sleep 1; done
EOF
    start
    before=$(count_at mark1)
    : > "$T/go1"
    after=$(count_at mark2)
    : > "$T/go2"
    wait "$SHELL_PID"
    rc=$?
    if [ "$rc" != 0 ] || [ ! -e "$T/mark2" ] || [ "$((after - before))" -gt "$ALLOWED" ]; then
        echo "FAIL $name: $before descriptors, then $after after $ROUNDS rounds, status $rc"
        head -5 "$T/err.txt"
        fails=$((fails + 1))
    else
        echo "ok $name: $before then $after"
    fi
}

# check <name> <script> <output>, with the status always 0.
check() {
    local name=$1 script=$2 want=$3 rc out
    setup
    printf '%s\n' "$script" > "$T/t.sh"
    start
    wait "$SHELL_PID"
    rc=$?
    out=$(cat "$T/out.txt")
    if [ "$rc" != 0 ] || [ "$out" != "$want" ]; then
        echo "FAIL $name: status $rc"
        printf 'want:\n%s\ngot:\n%s\nstderr:\n' "$want" "$out"
        head -5 "$T/err.txt"
        fails=$((fails + 1))
    else
        echo "ok $name"
    fi
}

# gVmVprocStdout
flat pascal_null 'pascal h.pas > /dev/null'
flat clike_file 'clike h.clike > o.txt'
# gVmVprocStdin, not leaving stdin's dup behind
flat clike_stdin 'clike r.clike < in.txt > /dev/null'
# A VM worker thread
flat clike_worker 'clike t.clike > o.txt'
# The shell's own family on a substitution's thread and through one
flat substitution 'x=$(echo y; clike h.clike)'
# A thread ended by a kill: the VM's streams and the shell's
flat killed_clike 'clike flood.clike > o.txt & p=$!; kill $p; wait $p'
flat killed_job '{ while :; do echo y; done; } > o.txt & p=$!; kill $p; wait $p'
# A thread ended by exit
flat exit_subshell '( echo y; exit 3 ) > o.txt'
flat exit_job '{ echo y; exit 3; } > o.txt & wait $!'

# A killed writer's status comes back with data still in its buffer.
check kill_file 'clike flood.clike > o.txt &
p=$!
sleep 1
kill $p
wait $p
echo "ended $?"' 'ended 143'

# fifo_held: how many descriptors the shell has on this case's FIFO, from lsof outside it.
fifo_held() {
    lsof -p "$SHELL_PID" 2>/dev/null | awk '$5 == "FIFO"' | grep -c "$(basename "$BASE")/r$n/f\$"
}

# A killed writer whose reader has stalled ends, and lets go of the FIFO while the reader still
# stalls. Its writes wait where a kill can act: libc's own write under a FILE can't be cancelled,
# and the thread stayed in it, holding 4 descriptors, until the reader moved.
setup
mkfifo "$T/f"
sleep 120 < "$T/f" &
READER=$!
printf '%s\n' 'clike flood.clike > f &
p=$!
sleep 1
kill $p
wait $p
echo "ended $?"
: > mark
while [ ! -e go ]; do sleep 1; done' > "$T/t.sh"
start
count_at mark > /dev/null
held=$(fifo_held)
i=0
while [ "$held" != 0 ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    held=$(fifo_held)
    i=$((i + 1))
done
: > "$T/go"
wait "$SHELL_PID"
rc=$?
out=$(cat "$T/out.txt")
kill "$READER" 2>/dev/null
wait "$READER" 2>/dev/null
READER=""
if [ "$rc" != 0 ] || [ "$out" != "ended 143" ] || [ "$held" != 0 ]; then
    echo "FAIL kill_stalled_reader: status $rc, $held descriptors left on the FIFO, output: $out"
    head -5 "$T/err.txt"
    fails=$((fails + 1))
else
    echo "ok kill_stalled_reader"
fi

# A FIFO's reader that goes away gives the writer EPIPE, not the whole shell a SIGPIPE: the
# shell's own signal() goes to its vproc, and the host's disposition ended it with 141.
setup
mkfifo "$T/f"
printf '%s\n' 'head -c 50 < f > /dev/null &
r=$!
clike flood.clike > f &
p=$!
wait $r
echo "reader $?"
sleep 1
kill $p
wait $p
echo "ended $?"' > "$T/t.sh"
start
wait "$SHELL_PID"
rc=$?
out=$(cat "$T/out.txt")
if [ "$rc" != 0 ] || [ "$out" != "reader 0
ended 143" ]; then
    echo "FAIL reader_leaves: status $rc, output: $out"
    head -5 "$T/err.txt"
    fails=$((fails + 1))
else
    echo "ok reader_leaves"
fi

# Later commands still have their stdout.
check after_kill 'clike flood.clike > o.txt &
p=$!
sleep 1
kill $p
wait $p
clike h.clike
pascal h.pas | cat
echo done' 'hello
hello
done'

if [ "$fails" != 0 ]; then
    echo "test_thread_end_streams: $fails of $n failed"
    exit 1
fi
echo "test_thread_end_streams: $n passed"
