#!/bin/bash
# What `lps` keeps after a kill, and what a kill leaves running. A task whose parent ended first has
# no one to wait for it, so its row goes when it ends; these stayed `exited` until the app restarted.
# And what waits for a frontend can be cancelled alone: the frontend ran on, with a row saying `exited`.
#   test_killed_rows.sh <exsh>
set -u
EXE=${1:?usage: test_killed_rows.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-killed-rows.XXXXXX") || exit 1
case "$BASE" in
    */exsh-killed-rows.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

fails=0
n=0

# check <name> <script> <output>, with the status always 0. Stops a case that hangs after 30 s.
check() {
    local name=$1 script=$2 want=$3
    n=$((n + 1))
    T="$BASE/r$n"
    mkdir -p "$T"
    printf 'program Spin;\nvar i: integer;\nbegin\n  i := 0;\n  while true do i := 1 - i;\nend.\n' > "$T/spin.pas"
    printf "program Slow;\nuses crt;\nbegin\n  delay(700);\n  writeln('slow done');\nend.\n" > "$T/slow.pas"
    printf "program Quick;\nbegin\n  writeln('quick done');\nend.\n" > "$T/quick.pas"
    printf '%s\n' "$script" > "$T/t.sh"
    local out rc
    # To a file: a frontend left running holds a pipe, and it may write there by itself much later.
    (cd "$T" && PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" /usr/bin/perl -e 'alarm 30; exec @ARGV' "$EXE" --no-cache t.sh > out.txt 2>/dev/null < /dev/null)
    rc=$?
    out=$(cat "$T/out.txt")
    if [ "$rc" != 0 ] || [ "$out" != "$want" ]; then
        echo "FAIL $name: status $rc"
        printf 'want:\n%s\ngot:\n%s\n' "$want" "$out"
        fails=$((fails + 1))
    else
        echo "ok $name"
    fi
}

check timeout_frontend '
timeout 0.3 pascal spin.pas
echo "rc $?"
lps | grep -c "[p]ascal spin"
exit 0' 'rc 124
0'

check timeout_frontend_in_a_pipeline '
timeout 0.3 pascal spin.pas | cat
lps | grep -c "[p]ascal spin"
exit 0' '0'

check timeout_shell_running_a_frontend '
timeout 0.3 sh -c "pascal spin.pas; echo late"
echo "rc $?"
sleep 0.2
lps | grep -c "[p]ascal spin"
exit 0' 'rc 124
0'

check subshells_job_that_ends '
( sleep 0.1 & )
sleep 0.4
lps | grep -c "[s]leep 0.1"
exit 0' '0'

check shells_job_that_ends '
sh -c "sleep 0.1 &"
sleep 0.4
lps | grep -c "[s]leep 0.1"
exit 0' '0'

check stages_job_that_ends '
sleep 0.01 | ( sleep 0.1 & )
sleep 0.4
lps | grep -c "[s]leep 0.1"
exit 0' '0'

check orphan_is_running_until_it_ends '
( sleep 0.5 & )
sleep 0.1
lps | grep "[s]leep 0.5" | grep -c running
kill -0 $(lps | grep "[s]leep 0.5" | cut -d" " -f1) && echo reachable
sleep 0.7
lps | grep -c "[s]leep 0.5"
exit 0' '1
reachable
0'

# The frontend would say slow done, and it holds the shell.
check killed_subshell_ends_its_frontend '
( pascal slow.pas ) &
j=$!
sleep 0.2
kill $j
wait $j
echo "rc $?"
sleep 0.9
lps | grep -c "[p]ascal slow"
exit 0' 'rc 143
0'

check killed_shell_ends_its_frontend '
sh -c "pascal slow.pas; echo late" &
j=$!
sleep 0.2
kill $j
wait $j
echo "rc $?"
sleep 0.9
lps | grep -c "[p]ascal slow"
exit 0' 'rc 143
0'

# A loop that makes no calls stops too, and frontends go on working after it.
check killed_subshell_ends_a_frontend_in_a_loop '
( pascal spin.pas ) &
j=$!
sleep 0.2
kill $j
wait $j
echo "rc $?"
sleep 0.1
lps | grep -c "[p]ascal spin"
pascal quick.pas
exit 0' 'rc 143
0
quick done'

# A kill as a frontend finishes: what it leaves must not stop the next one from finishing.
check frontend_after_kills_around_an_ending_one '
i=0
while [ $i -lt 8 ]; do
    timeout 0.2$i pascal quick.pas > /dev/null
    i=$((i + 1))
done
pascal quick.pas
exit 0' 'quick done'

# What shell code starts is the shell'"'"'s to wait for, however the name was looked up.
check wait_after_shell_code '
printf "(sleep 0.1; exit 6) &\n" > src.sh
eval "(sleep 0.1; exit 7) &"
wait $!
echo "eval $?"
. ./src.sh
wait $!
echo "dot $?"
command eval "(sleep 0.1; exit 7) &"
sleep 0.3
wait $!
echo "command eval $?"
command . ./src.sh
wait $!
echo "command dot $?"
command -p eval "(sleep 0.1; exit 7) &"
wait $!
echo "command -p $?"
command -- eval "(sleep 0.1; exit 7) &"
wait $!
echo "command -- $?"
builtin eval "(sleep 0.1; exit 7) &"
sleep 0.3
wait $!
echo "builtin eval $?"
builtin source ./src.sh
wait $!
echo "builtin source $?"
command builtin eval "(sleep 0.1; exit 7) &"
sleep 0.3
wait $!
echo "command builtin $?"
exit 0' 'eval 7
dot 6
command eval 7
command dot 6
command -p 7
command -- 7
builtin eval 7
builtin source 6
command builtin 7'

if [ "$fails" -ne 0 ]; then
    echo "test_killed_rows: $fails of $n failed"
    exit 1
fi
echo "test_killed_rows: $n passed"
