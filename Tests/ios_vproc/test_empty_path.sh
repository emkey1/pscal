#!/bin/bash
# An empty path names nothing: on the iOS-flavoured exsh each command below must fail as GNU's does and
# leave the virtual root alone. It used to mean the cwd, so `rm -rf ""` removed "/<child>" for each entry.
# Every case gets its own throwaway root, named by both PATH_TRUNCATE and HOME, and runs in a directory
# holding only a marker found nowhere else, so a regression has nothing real to remove.
#   test_empty_path.sh <exsh>
set -u
EXE=${1:?usage: test_empty_path.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-empty-path.XXXXXX") || exit 1
case "$BASE" in
    */exsh-empty-path.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

fails=0
n=0

listing() {
    (cd "$1" && ls -lA | awk 'NR > 1 { print $1, $NF }')
}

# check <command> <status> <output, or - to skip comparing it>
check() {
    local cmd=$1 want_rc=$2 want_out=$3
    n=$((n + 1))
    local T="$BASE/r$n" out rc before after problem=""
    mkdir -p "$T/sub"
    echo root > "$T/zz_empty_a"
    chmod 640 "$T/zz_empty_a"
    echo cwd > "$T/sub/zz_empty_a"

    # The mapping must be live before an empty path is tried, and this also creates what it provisions.
    out=$(cd "$T/sub" && PATH_TRUNCATE="$T" HOME="$T" "$EXE" -c 'cat /zz_empty_a' 2>&1)
    if [ "$out" != root ]; then
        echo "FAIL $cmd: the throwaway root is not mapped, got: $out"
        fails=$((fails + 1))
        return
    fi

    before=$(listing "$T"; listing "$T/sub")
    out=$(cd "$T/sub" && PATH_TRUNCATE="$T" HOME="$T" "$EXE" -c "$cmd" 2>&1)
    rc=$?
    after=$(listing "$T"; listing "$T/sub")

    [ "$rc" = "$want_rc" ] || problem="status $rc, wanted $want_rc"
    if [ "$want_out" != - ] && [ "$out" != "$want_out" ]; then
        problem="$problem output [$out], wanted [$want_out]"
    fi
    if [ "$before" != "$after" ]; then
        problem="$problem files changed: $(diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | tr '\n' ' ')"
    fi
    if [ -n "$problem" ]; then
        echo "FAIL $cmd: $problem"
        fails=$((fails + 1))
    else
        echo "ok   $cmd"
    fi
}

NOENT="No such file or directory"
check 'rm -rf ""'         0 ""
check 'rm -rf "$nosuch"'  0 ""
check 'rm -f ""'          0 ""
check 'rm -r ""'          1 "rm: cannot remove '': $NOENT"
check 'rm ""'             1 "rm: cannot remove '': $NOENT"
check 'rmdir ""'          1 "rmdir: failed to remove '': $NOENT"
check 'chmod -R 700 ""'   1 "chmod: cannot access '': $NOENT"
check 'ln -s zz_link ""'  1 "ln: failed to create symbolic link '': $NOENT"
check 'mv "" zz_new'      1 "mv: cannot stat '': $NOENT"
check 'cp -r "" zz_new'   1 "cp: cannot stat '': $NOENT"
check 'touch ""'          1 "touch: cannot touch '': $NOENT"
check 'mkdir ""'          1 -
check 'mkdir -p ""'       1 -
check 'ls ""'             2 "ls: cannot access '': $NOENT"
check 'cat ""'            1 "cat: '': $NOENT"
check 'find ""'           1 -
check 'stat ""'           1 -
check 'echo x > ""'       1 -
check 'cat < ""'          1 -
check 'cd ""'             0 ""

if [ "$fails" -ne 0 ]; then
    echo "test_empty_path: $fails of $n failed"
    exit 1
fi
echo "test_empty_path: all $n passed"
