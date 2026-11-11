#!/bin/bash
# A program writing to a pipe or FIFO whose reader has gone takes SIGPIPE from the kernel (vproc), as a
# process does on Linux, and by default ends with 141: VM frontends, applets, builtins in a subshell
# and the top-level shell itself. The app ignores SIGPIPE on the host, so such writes used to only fail,
# and a program that took no notice, as most don't, ran on for good: `clike flood | head -1` never
# ended. Under `trap '' PIPE` a write still gets EPIPE and the writer goes on, as on Linux. Each case
# runs in its own throwaway root, stopped after 90 s.
#   test_closed_pipe.sh <exsh>
set -u
EXE=${1:?usage: test_closed_pipe.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-closed-pipe.XXXXXX") || exit 1
case "$BASE" in
    */exsh-closed-pipe.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

fails=0
n=0
X=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

# check <name> <script> <output> [status]: the shell's status defaults to 0.
check() {
    local name=$1 script=$2 want=$3 want_rc=${4:-0} rc out
    n=$((n + 1))
    T="$BASE/r$n"
    mkdir -p "$T"
    printf 'int main() {\n    while (1) {\n        printf("%s\\n");\n    }\n    return 0;\n}\n' "$X" > "$T/flood.clike"
    printf "program Flood;\nbegin\n  while true do writeln('%s');\nend.\n" "$X" > "$T/flood.pas"
    printf 'int main() {\n    int i;\n    for (i = 0; i < 20000; i++) {\n        printf("line %%d\\n", i);\n    }\n    printf("done\\n");\n    return 0;\n}\n' > "$T/some.clike"
    mkfifo "$T/f"
    printf '%s\n' "$script" > "$T/t.sh"
    ( cd "$T" && PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" exec /usr/bin/perl -e 'alarm 90; exec @ARGV' "$EXE" --no-cache t.sh > out.txt 2> err.txt < /dev/null )
    rc=$?
    out=$(cat "$T/out.txt")
    if [ "$rc" != "$want_rc" ] || [ "$out" != "$want" ]; then
        echo "FAIL $name: status $rc, want $want_rc"
        printf 'want:\n%s\ngot:\n%s\nstderr:\n' "$want" "$out"
        head -5 "$T/err.txt"
        fails=$((fails + 1))
    else
        echo "ok $name"
    fi
}

# VM frontends, as a stage.
check clike_stage 'clike flood.clike | head -1
echo "${PIPESTATUS[*]}"' "$X
141 0"
check pascal_stage 'pascal flood.pas | head -1
echo "${PIPESTATUS[*]}"' "$X
141 0"
# An applet, as bash shows `yes | head -1`.
check applet_stage 'yes | head -1
echo "${PIPESTATUS[*]}"' "y
141 0"
# Its own status, inside a group that goes on, as under bash.
check frontend_in_group '{ clike flood.clike; echo "after $?" > after.txt; } | head -1
echo "${PIPESTATUS[*]}"
cat after.txt' "$X
0 0
after 141"
check applet_in_group '{ yes; echo "after $?" > after.txt; } | head -1
echo "${PIPESTATUS[*]}"
cat after.txt' "y
0 0
after 141"
# A builtin in a subshell is the subshell, which ends.
check subshell_builtin '( while :; do echo x; done; echo never > never.txt ) | head -1
echo "${PIPESTATUS[*]}"
if [ -e never.txt ]; then echo never; fi' "x
141 0"
check fifo_foreground 'head -c 5 < f > /dev/null &
r=$!
clike flood.clike > f
echo "fifo $?"
wait $r' 'fifo 141'
check fifo_job 'head -c 5 < f > /dev/null &
r=$!
clike flood.clike > f &
p=$!
wait $r
wait $p
echo "job $?"' 'job 141'
# The top-level shell's own builtin: the script ends there, with 141, as bash does.
check top_level_builtin 'exec 3> >(head -1 > /dev/null)
echo started
sleep 1
while :; do echo line >&3; done
echo survived' 'started' 141
# Ignored, a write only fails, and the writer goes on to its end.
check frontend_ignored '( trap "" PIPE; clike some.clike | head -1; echo "ignored ${PIPESTATUS[*]}" )' 'line 0
ignored 0 0'
check applet_ignored 'trap "" PIPE
yes | head -1
echo "ignored ${PIPESTATUS[*]}"' 'y
ignored 1 0'
check top_level_ignored 'trap "" PIPE
exec 3> >(head -1 > /dev/null)
sleep 1
for i in 1 2 3; do sleep 0.5; echo line >&3; done
echo "went on $?"' 'went on 1'
# Trapped, the trap runs and the write fails, as under bash.
check top_level_trapped 'trap "echo caught >> c.txt" PIPE
exec 3> >(head -1 > /dev/null)
sleep 1
for i in 1 2 3; do sleep 0.5; echo line >&3; done 2>/dev/null
echo "went on $?"
sort -u c.txt' 'went on 1
caught'
check subshell_ignored '( trap "" PIPE; for i in 1 2 3 4 5; do sleep 0.2; echo x; done; echo "went on" > on.txt ) | head -1
cat on.txt' 'x
went on'
# A reader that reads it all sees no difference.
check whole 'clike some.clike | tail -1
echo "${PIPESTATUS[*]}"' 'done
0 0'

if [ "$fails" != 0 ]; then
    echo "test_closed_pipe: $fails of $n failed"
    exit 1
fi
echo "test_closed_pipe: $n passed"
