#!/bin/bash
# $HOME is the app's name for the home directory on the iOS-flavoured exsh, as pwd, realpath and
# /proc/self/fd give it: started with a host $HOME under the root (.../Documents/home in the app),
# the shell names it /home. A $HOME that is the root itself, as on a plain run of this flavour,
# stays as it is. Each run gets its own throwaway root, named by PATH_TRUNCATE.
#   test_virtual_home.sh <exsh>
set -u
EXE=${1:?usage: test_virtual_home.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-virtual-home.XXXXXX") || exit 1
case "$BASE" in
    */exsh-virtual-home.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT
# The name the shell sees for the root, which may differ from mktemp's spelling (/var, /private/var).
T=$(cd "$BASE" && pwd -P)
mkdir -p "$T/home/sub" "$T/tmp"
echo needle > "$T/home/f"

fails=0
n=0

# check <home> <command> <output>
check() {
    local home=$1 cmd=$2 want=$3 out
    n=$((n + 1))
    out=$(cd "$T/home" && env -u PSCALI_CONTAINER_ROOT -u PSCALI_REAL_HOME -u PSCALI_HOME \
        PATH_TRUNCATE="$T" HOME="$home" TMPDIR="$T/tmp" PATH="$T/bin:$PATH" SESSIONPATH="$T/tmp:~:." \
        "$EXE" --no-cache -c "$cmd" 2>&1 < /dev/null)
    if [ "$out" != "$want" ]; then
        echo "FAIL $cmd: got [$out], wanted [$want]"
        fails=$((fails + 1))
    else
        echo "ok   $cmd"
    fi
}

check "$T/home" 'echo "$HOME"; echo ~'                      $'/home\n/home'
check "$T/home" 'cd /; cd; pwd; echo "$PWD"'                $'/home\n/home'
check "$T/home" 'readlink -f ~/f; realpath ~'               $'/home/f\n/home'
check "$T/home" 'exec 3< ~/f; readlink /proc/self/fd/3'     '/home/f'
check "$T/home" 'cd ~/sub; dirs'                            '~/sub'
check "$T/home" 'cat ~/f; cat "$HOME/f"'                    $'needle\nneedle'
check "$T/home" '[ "$(cd; pwd)" = "$HOME" ] && [ "$(readlink -f ~)" = "$HOME" ] && echo agree' 'agree'
check "$T/home" 'sh -c '\''echo "$HOME"'\'''               '/home'
# cd with no operand goes to $HOME, whatever it is set to, as in bash.
check "$T/home" 'HOME=/home/sub; cd; pwd'                   '/home/sub'
# $TMPDIR, $PATH and $SESSIONPATH take the app's names too.
check "$T/home" 'echo "$TMPDIR"; echo "${PATH%%:*}"; echo "$SESSIONPATH"' $'/tmp\n/bin\n/tmp:~:.'
# Temporary files, times and links on an app path, which reached the host's /home.
check "$T/home" 'cp ~/f ~/g; sed -i s/needle/pin/ ~/g; cat ~/g; ls ~ | grep -c "^sed"' $'pin\n0'
check "$T/home" ': > ~/t; touch -r ~/f ~/t; echo $?; [ ~/t -nt ~/f ] || [ ~/f -nt ~/t ] || echo same' $'0\nsame'
check "$T/home" 'rm -f ~/c; cp -p ~/f ~/c; echo $?; [ ~/c -nt ~/f ] || [ ~/f -nt ~/c ] || echo same' $'0\nsame'
check "$T/home" 'rm -f ~/l; ln -L ~/f ~/l; echo $?; cat ~/l' $'0\nneedle'
# The same directory by another spelling of the root.
case "$T" in
    /private/*) check "${T#/private}/home" 'echo "$HOME"' '/home' ;;
esac
# A home that is the root itself keeps its host name: "/" would name every path.
check "$T" 'echo "$HOME"'                                   "$T"
# A home outside the root is left alone.
check "/nonexistent-home-$$" 'echo "$HOME"'                 "/nonexistent-home-$$"

if [ "$fails" -ne 0 ]; then
    echo "test_virtual_home: $fails of $n failed"
    exit 1
fi
echo "test_virtual_home: all $n passed"
