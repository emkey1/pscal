#!/bin/bash
# Killing the stages of a foreground pipeline leaves the shell going. The last stage runs on the shell's
# thread, and a thread the shell had started hid that, so the kill cancelled the shell: only the kernel's
# thread was left, which blocks every signal, so nothing ended the process short of SIGKILL. In the app
# that is a tab with no prompt. The task numbers are small and repeat on this flavour, so the script
# signals all of them but its own and the shell's. A killed first stage let go of its end of the pipe only
# when it returned, so the last, still reading, waited for good.
#   test_kill_last_stage.sh <exsh>
set -u
EXE=${1:?usage: test_kill_last_stage.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-kill-last.XXXXXX") || exit 1
case "$BASE" in
    */exsh-kill-last.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

fails=0
check() { # name script want
    local name=$1 script=$2 want=$3
    printf '%s\n' "$script" > "$BASE/t.sh"
    ( cd "$BASE" && PATH_TRUNCATE="$BASE" HOME="$BASE" TMPDIR="$BASE" exec "$EXE" --no-cache t.sh ) > "$BASE/out" 2> "$BASE/err" < /dev/null &
    local pid=$! n=0
    while kill -0 "$pid" 2> /dev/null && [ "$n" -lt 150 ]; do
        sleep 0.1
        n=$((n + 1))
    done
    if kill -0 "$pid" 2> /dev/null; then
        kill -9 "$pid" 2> /dev/null
        wait "$pid" 2> /dev/null
        echo "FAIL $name: the shell never went on"
        fails=$((fails + 1))
        return
    fi
    wait "$pid" 2> /dev/null
    local got
    got=$(cat "$BASE/out")
    if [ "$got" != "$want" ]; then
        echo "FAIL $name"
        printf 'want:\n%s\ngot:\n%s\nstderr:\n' "$want" "$got"
        sed -n 1,5p "$BASE/err"
        fails=$((fails + 1))
    else
        echo "ok $name"
    fi
}

KILL_STAGES='for i in 3 4 5 6 7 8 9 10 11 12; do [ "$i" = "$BASHPID" ] || [ "$i" = "$$" ] || kill -TERM "$i" 2> /dev/null; done'
check "a foreground pipeline" "( sleep 0.3; $KILL_STAGES ) & sleep 2 | sleep 2; echo alive; echo more" "alive
more"
check "a pipeline of three" "( sleep 0.3; $KILL_STAGES ) & sleep 2 | sleep 2 | sleep 2; echo alive" "alive"
check "again after one" "sleep 0 | sleep 0; ( sleep 0.3; $KILL_STAGES ) & sleep 2 | sleep 2; echo alive" "alive"
FIRST='( sleep 0.3; lps | grep "[s]leep 2.3" | while read kp kr; do [ "$kp" = $$ ] || kill -TERM "$kp"; done ) & sleep 2.3 | cat; echo "st ${PIPESTATUS[0]}"'
check "the first stage, with the last still reading" "$FIRST" "st 143"
[ "$fails" = 0 ] && echo "test_kill_last_stage: ok"
exit "$fails"
