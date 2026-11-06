#!/bin/bash
# A subshell killed by its own job while it waits. Darwin can act on that cancel inside the wait after
# letting go of the task table's mutex, and the wait's cleanup then unlocked another thread's hold on
# it: a later wait ended the process as a corrupt mutex, with SIGKILL. One run of the loop hit that
# about one time in six, two side by side more than half the time, so this runs pairs.
#   test_cancel_wait_race.sh <exsh>
set -u
EXE=${1:?usage: test_cancel_wait_race.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-cancel-wait.XXXXXX") || exit 1
case "$BASE" in
    */exsh-cancel-wait.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

cat > "$BASE/t.sh" <<'SCRIPT'
i=0
while [ $i -lt 150 ]; do
  i=$((i + 1))
  ( p=$BASHPID; kill -TERM $p & wait $!; echo no )
  ( p=$BASHPID; ( sleep 0.01; kill -TERM $p ) & wait; echo no )
done
echo "done $i"
SCRIPT

fails=0
start() { # name: the process lands in $!
    mkdir -p "$BASE/$1"
    cp "$BASE/t.sh" "$BASE/$1/t.sh"
    ( cd "$BASE/$1" && PATH_TRUNCATE="$BASE/$1" HOME="$BASE/$1" TMPDIR="$BASE/$1" exec "$EXE" --no-cache t.sh ) > "$BASE/$1.out" 2> "$BASE/$1.err" < /dev/null &
}
finish() { # pid name
    local pid=$1 name=$2 n=0 rc got
    while kill -0 "$pid" 2> /dev/null && [ "$n" -lt 900 ]; do
        sleep 0.1
        n=$((n + 1))
    done
    if kill -0 "$pid" 2> /dev/null; then
        kill -9 "$pid" 2> /dev/null
        wait "$pid" 2> /dev/null
        echo "FAIL run $name: still running after 90 s"
        fails=$((fails + 1))
        return
    fi
    wait "$pid"
    rc=$?
    got=$(cat "$BASE/$name.out")
    if [ "$rc" != 0 ] || [ "$got" != "done 150" ]; then
        echo "FAIL run $name: status $rc, output '$got'"
        sed -n 1,5p "$BASE/$name.err"
        fails=$((fails + 1))
    fi
}
for round in 1 2 3; do
    start "$round.a"
    a=$!
    start "$round.b"
    b=$!
    finish "$a" "$round.a"
    finish "$b" "$round.b"
done
if [ "$fails" -gt 0 ]; then
    echo "test_cancel_wait_race: $fails of 6 runs failed"
    exit 1
fi
echo "test_cancel_wait_race: ok"
