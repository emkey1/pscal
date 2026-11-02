#!/bin/bash
# A VM program waiting for its input neither stops the rest of the app nor ends it when killed.
# Killing a background pipeline cancels the thread that started the program, and not the worker
# running it, whose next read followed a pointer into that thread's stack: SIGSEGV. `eof` waited
# holding the lock every VM takes to start, so nothing else ran until its input came, the shell's
# own $(...) included. Each case runs in its own throwaway root.
#   test_kill_vm_reader.sh <exsh>
set -u
EXE=${1:?usage: test_kill_vm_reader.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-kill-vm.XXXXXX") || exit 1
case "$BASE" in
    */exsh-kill-vm.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT

fails=0
n=0

# check <name> <script> <output>, with the status always 0. Stops a case that hangs after 20 s.
check() {
    local name=$1 script=$2 want=$3
    n=$((n + 1))
    T="$BASE/r$n"
    mkdir -p "$T"
    printf 'int main() {\n    str s;\n    readln(stdin, s);\n    printf("[%%s]\\n", s);\n    return 0;\n}\n' > "$T/r.clike"
    printf 'int main() {\n    str s;\n    while (!eof(stdin)) {\n        readln(stdin, s);\n        printf("[%%s]\\n", s);\n    }\n    return 0;\n}\n' > "$T/e.clike"
    printf 'int main() {\n    printf("hello\\n");\n    return 0;\n}\n' > "$T/h.clike"
    printf "program R;\nvar s: string;\nbegin\n  readln(s);\n  writeln('[', s, ']');\nend.\n" > "$T/r.pas"
    : > "$T/tf"
    printf '%s\n' "$script" > "$T/t.sh"
    local out rc
    # To a file: a pipe would wait for anything the script left behind.
    (cd "$T" && PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" /usr/bin/perl -e 'alarm 20; exec @ARGV' "$EXE" --no-cache t.sh > out.txt 2>/dev/null < /dev/null)
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

check killed_in_readln '
sleep 300 | clike r.clike > /dev/null &
j=$!
sleep 1
kill $j
wait $j
echo "wait $?"
clike h.clike
exit 0' 'wait 143
hello'

check killed_in_pascal_readln '
sleep 300 | pascal r.pas > /dev/null &
j=$!
sleep 1
kill $j
wait $j
echo "wait $?"
clike h.clike
exit 0' 'wait 143
hello'

check killed_in_eof '
sleep 300 | clike e.clike > /dev/null &
j=$!
sleep 1
kill $j
wait $j
echo "wait $?"
clike h.clike
exit 0' 'wait 143
hello'

check others_run_while_eof_waits '
tail -f tf | clike e.clike > /dev/null &
j=$!
sleep 1
x=$(echo substitution)
echo "$x"
clike h.clike
exsh -c "echo nested"
kill $j
wait $j
echo "wait $?"
exit 0' 'substitution
hello
nested
wait 143'

check nothing_left_after_kill '
sleep 300 | clike r.clike > /dev/null &
j=$!
sleep 1
kill $j
wait $j
sleep 0.5
ps | grep -c "r\.clike"
exit 0' '0'

check reads_its_pipe '
printf "a\nb\n" | clike e.clike
exit 0' '[a]
[b]'

check reads_its_file '
printf "a\nb\n" > f
clike e.clike < f
pascal r.pas < f
exit 0' '[a]
[b]
[a]'

# Not a kill: a #! program in a pipe runs on the shell's thread and leaves it with no global table,
# and the analysis of the next nested shell's command said "Internal error" on stderr.
check nested_shell_after_shebang_in_a_pipe '
printf "#!/usr/bin/env pascal\nprogram H;\nbegin\n  writeln(1);\nend.\n" > h
chmod +x h
mkdir d
printf "#!/bin/exsh\necho inner\n" > d/s.sh
chmod +x d/s.sh
echo x | ./h > /dev/null
sh -c "cd d; ./s.sh" 2>&1
exit 0' 'inner'

echo "kill_vm_reader: $n cases, $fails failed"
[ "$fails" -eq 0 ]
