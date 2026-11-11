#!/bin/bash
# Real keystrokes at the PSCAL app's real prompt, in an iOS simulator: one case for each keyboard
# form that only a person at the keys can show (typed lines, Ctrl-C, Ctrl-D). Not run by CI.
# See README.md.
#
# Usage: ios/KeyTests/keys.sh --app PATH/PSCAL.app --device UDID [--work DIR] [CASE ...]
#        ios/KeyTests/keys.sh --list
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
APP=""
UD=""
WORK="${TMPDIR:-/tmp}/pscal-keytests"
CASES=()
ALL_CASES="eof_readln_loop eof_after_kill eof_ctrl_c_status readln_ctrl_c_quiet delay_ctrl_c wc_ctrl_c sort_ctrl_c pipe_ctrl_c utf8_typed micro_task_mask fd_names_at_prompt"

while [ $# -gt 0 ]; do
    case $1 in
        --app) APP=$2; shift 2 ;;
        --device) UD=$2; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        --list) printf '%s\n' $ALL_CASES; exit 0 ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) CASES+=("$1"); shift ;;
    esac
done
[ -d "$APP" ] && [ -n "$UD" ] || { sed -n '2,8p' "$0"; exit 2; }
[ ${#CASES[@]} -gt 0 ] || read -r -a CASES <<< "$ALL_CASES"

CMD=$WORK/cmd      # the driver's command directory
BUILD=$WORK/build  # the driver's build
SHOTS=$WORK/shots
mkdir -p "$WORK" "$SHOTS"

# --- The driver: an XCUITest that types what it finds in $CMD/cmd.N and answers in $CMD/res.N.

build_driver() {
    ls "$BUILD"/Build/Products/*.xctestrun > /dev/null 2>&1 && return 0
    echo "building the driver into $BUILD"
    xcodebuild build-for-testing -project "$HERE/KeyTests.xcodeproj" -scheme KeyDriver \
        -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath "$BUILD" \
        CODE_SIGNING_ALLOWED=YES CODE_SIGN_IDENTITY=- > "$WORK/driver-build.log" 2>&1 ||
        { echo "driver build failed: $WORK/driver-build.log"; exit 1; }
}

stop_driver() {
    if [ -f "$WORK/driver.pid" ]; then
        kill "$(cat "$WORK/driver.pid")" 2>/dev/null
        rm -f "$WORK/driver.pid"
    fi
}

start_driver() {
    stop_driver
    rm -rf "$CMD"
    mkdir -p "$CMD"
    local run
    run=$(ls "$BUILD"/Build/Products/*.xctestrun | head -1)
    TEST_RUNNER_KEYTESTS_DIR="$CMD" nohup xcodebuild -xctestrun "$run" -destination "id=$UD" \
        -parallel-testing-enabled NO test-without-building > "$WORK/driver.log" 2>&1 &
    echo $! > "$WORK/driver.pid"
    local i=0
    while [ "$i" -lt 90 ] && ! grep -q 'testDrive' "$WORK/driver.log"; do
        sleep 2
        i=$((i + 1))
    done
    grep -q 'testDrive' "$WORK/driver.log" || { echo "the driver did not start: $WORK/driver.log"; exit 1; }
}

ui() { # one driver command per argument; prints each answer
    local c n i
    for c in "$@"; do
        n=$(cat "$CMD/next" 2>/dev/null || echo 0)
        printf '%s\n' "$c" > "$CMD/.tmp.$$"
        mv "$CMD/.tmp.$$" "$CMD/cmd.$n"
        echo $((n + 1)) > "$CMD/next"
        i=0
        while [ ! -f "$CMD/res.$n" ] && [ "$i" -lt 1500 ]; do
            sleep 0.1
            i=$((i + 1))
        done
        if [ -f "$CMD/res.$n" ]; then head -c 300 "$CMD/res.$n"; else echo "TIMEOUT"; fi
    done
}

# --- The app, freshly installed, with the fixtures in ~/kt.

xcrun simctl boot "$UD" 2>/dev/null
xcrun simctl bootstatus "$UD" -b > /dev/null 2>&1
xcrun simctl terminate "$UD" com.pscal.PSCAL 2>/dev/null
xcrun simctl uninstall "$UD" com.pscal.PSCAL 2>/dev/null
xcrun simctl install "$UD" "$APP" || { echo "install failed"; exit 1; }
H="$(xcrun simctl get_app_container "$UD" com.pscal.PSCAL data)/Documents/home"
mkdir -p "$H/kt"
cat > "$H/kt/loop.clike" <<'EOF'
int main() {
    str s;
    while (!eof(stdin)) {
        readln(stdin, s);
        printf("[%s]\n", s);
    }
    return 0;
}
EOF
cat > "$H/kt/ask.pas" <<'EOF'
program Ask;
var s: string;
begin
  write('name? ');
  readln(s);
  writeln('got ', s);
end.
EOF
cat > "$H/kt/dly.pas" <<'EOF'
program Dly;
uses crt;
begin
  delay(30000);
  writeln('late');
end.
EOF

build_driver
start_driver
trap 'ui quit > /dev/null 2>&1; stop_driver' EXIT

# --- Helpers for the cases.

FAILS=0
run() { ui "$@" > /dev/null; }
type_line() { run "text $1\\n"; }
ctrl() { # Ctrl-<key>: the keyboard bar's Control, else a hardware keyboard's
    local out
    out=$(ui "tap Control" "softkey $1")
    case "$out" in *"no key"*|*"no button"*) run "ctrl $1" ;; esac
}
fresh() { run launch; sleep 4; type_line "cd kt"; sleep 1; rm -f "$H/kt"/*.out; }
got() { [ -f "$H/kt/$1" ] && tr '\n' '~' < "$H/kt/$1"; }
wait_for() { local i=0; while [ ! -s "$H/kt/$1" ] && [ "$i" -lt "${2:-20}" ]; do sleep 1; i=$((i + 1)); done; }
status() { type_line 'echo "rc $?" > rc.out'; wait_for rc.out 20; got rc.out; }
check() { # name, what it got, what it should be
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1: want [$3] got [$2]"
        FAILS=$((FAILS + 1))
        run "shot $1"
        cp "$CMD/$1.png" "$SHOTS/" 2>/dev/null
    fi
}
prompt_answers() { # the prompt runs the next line typed
    type_line 'echo next-$((6*7)) > next.out'
    wait_for next.out 20
    check "$1, then the prompt" "$(got next.out)" "next-42~"
}

# --- One case per form. Each starts the app afresh, so one stuck program can't take the next case's keys.

case_eof_readln_loop() { # eof and readln at the keyboard share the typed lines, and Ctrl-D ends the loop
    fresh
    type_line 'clike loop.clike > a.out 2>&1'; sleep 3
    type_line x; sleep 2
    type_line y; sleep 2
    ctrl d; sleep 3
    check eof_readln_loop "$(got a.out)" "[x]~[y]~"
    prompt_answers eof_readln_loop
}

case_eof_after_kill() { # a program killed in eof leaves the next reader of the keyboard its input
    fresh
    type_line "( sleep 3; kill \$(ps | grep 'clike loop' | grep -v grep | awk '{print \$1}') ) & clike loop.clike > a.out 2>&1"
    sleep 9
    type_line 'clike loop.clike > b.out 2>&1'; sleep 3
    type_line q; sleep 2
    ctrl d; sleep 3
    check eof_after_kill "$(got b.out)" "[q]~"
    prompt_answers eof_after_kill
}

case_eof_ctrl_c_status() { # Ctrl-C during eof(stdin) gives 130
    fresh
    type_line 'clike loop.clike > a.out 2>&1'; sleep 4
    ctrl c; sleep 3
    check eof_ctrl_c_status "$(status)" "rc 130~"
    prompt_answers eof_ctrl_c_status
}

case_readln_ctrl_c_quiet() { # Ctrl-C on a Pascal readln ends it with 130 and prints no VM dump
    fresh
    type_line 'pascal ask.pas > a.out 2>&1'; sleep 4
    ctrl c; sleep 3
    check readln_ctrl_c_quiet "$(status)" "rc 130~"
    check "readln_ctrl_c_quiet, output" "$(got a.out)" "name? "
}

case_delay_ctrl_c() { # Ctrl-C ends delay() at once
    fresh
    type_line 'pascal dly.pas > a.out 2>&1'; sleep 4
    ctrl c; sleep 2
    check delay_ctrl_c "$(status)" "rc 130~"
    check "delay_ctrl_c, output" "$(got a.out)" ""
    prompt_answers delay_ctrl_c
}

case_wc_ctrl_c() { # wc prints nothing on Ctrl-C
    fresh
    type_line 'wc > a.out 2>&1'; sleep 3
    type_line 'one two'; sleep 2
    ctrl c; sleep 3
    check wc_ctrl_c "$(status)" "rc 130~"
    check "wc_ctrl_c, output" "$(got a.out)" ""
}

case_sort_ctrl_c() { # sort prints nothing on Ctrl-C
    fresh
    type_line 'sort > a.out 2>&1'; sleep 3
    type_line b; sleep 1
    type_line a; sleep 2
    ctrl c; sleep 3
    check sort_ctrl_c "$(status)" "rc 130~"
    check "sort_ctrl_c, output" "$(got a.out)" ""
}

case_pipe_ctrl_c() { # Ctrl-C on `cat | tr` gives 130, and the rest of the line doesn't run
    fresh
    type_line 'cat | tr a b > a.out 2>&1; echo ran-on > r.out'; sleep 4
    ctrl c; sleep 3
    check pipe_ctrl_c "$(status)" "rc 130~"
    check "pipe_ctrl_c, rest of the line" "$(got r.out)" ""
}

case_utf8_typed() { # characters beyond ASCII at the prompt go in whole, not dropped with a bell
    fresh
    type_line 'echo é⚡x > u.out'
    wait_for u.out 20
    check utf8_typed "$(got u.out)" "é⚡x~"
    # A character at a time, with an emoji, and Backspace taking it whole.
    run 'chars echo ü😀日😀'
    sleep 4
    run 'chars \b'
    type_line ' > v.out'
    wait_for v.out 20
    check "utf8_typed, a character at a time" "$(got v.out)" "ü😀日~"
    prompt_answers utf8_typed
}

case_micro_task_mask() { # micro (Go) makes a new file with its own task's mask, not the first tab shell's
    fresh
    type_line 'umask 022; (umask 077; micro mk.txt)'; sleep 8
    run "text hi"; sleep 1
    ctrl s; sleep 3
    ctrl q; sleep 4
    type_line 'ls -l mk.txt | cut -c1-10 > mk.out'
    wait_for mk.out 20
    check micro_task_mask "$(got mk.out)" "-rw-------~"
    prompt_answers micro_task_mask
}

case_fd_names_at_prompt() { # /proc/$$/fd in a redirected group, typed at the prompt, not from a sourced file
    fresh
    type_line '{ echo a > /proc/$$/fd/1; } > pg.out'
    wait_for pg.out 10
    check "fd_names_at_prompt, /proc/\$\$ in a redirected group" "$(got pg.out)" "a~"
    prompt_answers fd_names_at_prompt
}

for c in "${CASES[@]}"; do
    case " $ALL_CASES " in
        *" $c "*) "case_$c" ;;
        *) echo "unknown case $c"; FAILS=$((FAILS + 1)) ;;
    esac
done
echo "keys: ${#CASES[@]} cases, $FAILS failed checks"
[ "$FAILS" -eq 0 ]
