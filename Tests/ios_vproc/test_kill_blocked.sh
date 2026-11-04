#!/bin/bash
# A killed command ends even when it is blocked on a FIFO, and `wait` reports it only once it has.
# The host's read, write and open in the app cannot be cancelled, so the command's thread stayed
# in them: `wait` said 143 while it was still there, as a reader that took the next writer's data
# or closed under it. Each case runs in its own throwaway root. mkfifo is the host's: the app has none.
#   test_kill_blocked.sh <exsh>
set -u
EXE=${1:?usage: test_kill_blocked.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-kill-blocked.XXXXXX") || exit 1
case "$BASE" in
    */exsh-kill-blocked.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

fails=0
n=0

# check <name> <script> <output>, with the status always 0. Stops a case that hangs after 15 s.
check() {
    local name=$1 script=$2 want=$3
    n=$((n + 1))
    T="$BASE/r$n"
    mkdir -p "$T"
    mkfifo "$T/p"
    printf '%s\n' "$script" > "$T/t.sh"
    local out rc
    # To a file: a pipe would wait for anything the script left blocked on the FIFO.
    (cd "$T" && PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" /usr/bin/perl -e 'alarm 15; exec @ARGV' "$EXE" --no-cache t.sh > out.txt 2>/dev/null < /dev/null)
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

check blocked_opening_to_read '
cat p > c.out &
j=$!
sleep 0.3
kill $j
wait $j
echo "wait $?"
( echo hi > p; echo done > w ) &
sleep 0.5
if [ -f w ]; then echo "a reader was left"; else echo "no reader"; fi
if [ -s c.out ]; then echo "the killed cat wrote"; fi
echo end
exit 0' 'wait 143
no reader
end'

check blocked_opening_to_write '
echo lost > p &
j=$!
sleep 0.3
kill $j
wait $j
echo "wait $?"
cat p > c.out &
r=$!
sleep 0.5
if [ -s c.out ]; then echo "the killed echo wrote"; else echo "nothing came"; fi
kill $r
wait $r
echo end
exit 0' 'wait 143
nothing came
end'

check blocked_reading '
exec 3<>p
cat p > c.out &
j=$!
sleep 0.3
kill $j
wait $j
echo "wait $?"
echo hi >&3
sleep 0.5
if [ -s c.out ]; then echo "the killed cat wrote"; else echo "not written"; fi
read line <&3
echo "$line"
exit 0' 'wait 143
not written
hi'

check blocked_writing '
exec 3<>p
( while :; do echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx; done ) > p &
j=$!
sleep 0.3
kill $j
wait $j
echo "wait $?"
exit 0' 'wait 143'

# What a FIFO has always done must still work: either end may come first, and an end of file reaches
# a reader whose writer came and went. Writes over PIPE_BUF arrive whole and in order.
check writer_first '
echo hi > p &
sleep 0.2
cat p
wait
echo "wait $?"' 'hi
wait 0'

check reader_first '
cat p &
sleep 0.2
echo hi > p
wait
echo "wait $?"' 'hi
wait 0'

check end_of_file_only '
cat p &
sleep 0.2
: > p
wait
echo "wait $?"' 'wait 0'

check long_write_whole '
echo 0123456789012345678901234567890123456789012345678901234567890123 > f
i=0
while [ $i -lt 11 ]; do cat f f > g; mv g f; i=$((i+1)); done
cat f > p &
cat p > o
wait
echo "wait $?"' 'wait 0'
if ! cmp -s "$T/f" "$T/o"; then
    echo "FAIL long_write_whole: the file did not come through whole"
    fails=$((fails + 1))
fi

# A job killed while it starts a pipeline: a stage thread was using the job's vproc as the job
# destroyed it. In the app that ends every tab. It takes about 3 runs in 100, so it needs many.
n=$((n + 1))
T="$BASE/r$n"
mkdir -p "$T"
printf '%s\n' '( while :; do echo hi | cat > /dev/null; done ) & p=$!; sleep 0.02; kill $p; wait $p; echo "wait $?"' > "$T/t.sh"
bad=0
last=
i=0
while [ $i -lt 300 ]; do
    i=$((i + 1))
    (cd "$T" && PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" /usr/bin/perl -e 'alarm 15; exec @ARGV' "$EXE" --no-cache t.sh > out.txt 2>/dev/null < /dev/null)
    rc=$?
    if [ "$rc" != 0 ] || [ "$(cat "$T/out.txt")" != "wait 143" ]; then
        bad=$((bad + 1))
        last="status $rc"
    fi
done
if [ "$bad" != 0 ]; then
    echo "FAIL killed_starting_pipeline: $bad of 300, the last with $last"
    fails=$((fails + 1))
else
    echo "ok killed_starting_pipeline"
fi

echo "kill_blocked: $n cases, $fails failed"
[ "$fails" -eq 0 ]
