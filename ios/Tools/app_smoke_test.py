#!/usr/bin/env python3
"""Run commands inside the iOS app in a simulator and compare their output.

CI only compiles the app and no host suite runs its commands, so code can be
broken on iOS with every suite green. Run this on a Release simulator build
before any TestFlight upload (see ios/README.md).

No UI is driven. The app is installed fresh, ~/.exshrc in its container gets a
line that sources smoke/auto.sh, and the app is launched. That script runs each
command with its output redirected to a result file and touches a sentinel at
the end. The host then reads the result files from the container. Afterwards
the app is uninstalled, and a simulator this run booted is shut down.

Failures listed under Known Issues in the release notes are in KNOWN, below, and
do not count against a run.

Exit status: 0 clean; 1 an unexpected result, a new crash report, or the app died
or stopped making progress; 2 the run could not be set up.
"""
from __future__ import annotations

import argparse
import json
import os
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import List, NamedTuple, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[2]
RELEASE_BUILD = "Build/Products/Release-iphonesimulator/PSCAL.app"
REPORTS = Path.home() / "Library/Logs/DiagnosticReports"
DEVICES = Path.home() / "Library/Developer/CoreSimulator/Devices"
HOOK = '[ -f "$HOME/smoke/auto.sh" ] && . "$HOME/smoke/auto.sh"'
# Lines the runtime logs into a command's captured stderr.
NOISE = re.compile(r"^(objc\[\d+\]: |\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ PSCAL\[).*\n", re.M)

R = "> {o} 2>&1"
A = ">> {o} 2>&1"

# (name, command, expected output). {o} is the test's result file. With an
# expected output of None, the result file only has to appear.
#
# Each command redirects itself, so its result lands in its own file. In a pipeline
# the redirect goes on the last stage.
TESTS: List[Tuple[str, str, Optional[str]]] = [
    ("cat_file", f"cat smoke/d.txt {R}", "b\na\nc\n"),
    ("cat_stdin", f"cat < smoke/d.txt {R}", "b\na\nc\n"),
    ("echo_pipe_cat", f"echo hi | cat {R}", "hi\n"),
    ("echo_pipe_od", f"echo hi | od -c | head -1 {R}", "0000000   h   i  \\n\n"),
    ("printf_pipe_cat", f"printf 'x\\ny\\n' | cat {R}", "x\ny\n"),
    ("head_file", f"head -1 smoke/d.txt {R}", "b\n"),
    ("head_n", f"head -n 2 smoke/d.txt {R}", "b\na\n"),
    ("head_stdin", f"cat smoke/d.txt | head -1 {R}", "b\n"),
    ("tail_file", f"tail -1 smoke/d.txt {R}", "c\n"),
    ("tail_stdin", f"cat smoke/d.txt | tail -1 {R}", "c\n"),
    ("wc_l_file", f"wc -l smoke/d.txt {R}", "3 smoke/d.txt\n"),
    ("wc_l_stdin", f"cat smoke/d.txt | wc -l {R}", "3\n"),
    ("wc_c_echo", f"echo abc | wc -c {R}", "4\n"),
    ("sort_file", f"sort smoke/d.txt {R}", "a\nb\nc\n"),
    ("sort_stdin", f"cat smoke/d.txt | sort {R}", "a\nb\nc\n"),
    ("sort_r", f"sort -r smoke/d.txt {R}", "c\nb\na\n"),
    ("sort_o", f"sort -o smoke/so.txt smoke/d.txt; cat smoke/so.txt {R}", "a\nb\nc\n"),
    ("uniq_file", f"uniq smoke/u.txt {R}", "a\nb\n"),
    ("uniq_c", f"uniq -c smoke/u.txt {R}", "      2 a\n      1 b\n"),
    ("cut_file", f"cut -d: -f2 smoke/c.txt {R}", "two\n"),
    ("tr_stdin", f"echo abc | tr a-z A-Z {R}", "ABC\n"),
    ("sed_file", f"sed s/a/z/ smoke/d.txt {R}", "b\nz\nc\n"),
    ("sed_stdin", f"echo abc | sed s/a/z/ {R}", "zbc\n"),
    ("sed_print", f"sed -n 2p smoke/d.txt {R}", "a\n"),
    ("sed_inplace", f"cp smoke/d.txt smoke/si.txt; sed -i s/a/q/ smoke/si.txt; cat smoke/si.txt {R}", "b\nq\nc\n"),
    ("awk_begin", f"awk 'BEGIN {{ print 6*7 }}' {R}", "42\n"),
    ("awk_file", f"awk '{{print $1}}' smoke/c.txt {R}", "one:two\n"),
    ("awk_stdin", f"echo 1 2 | awk '{{print $2}}' {R}", "2\n"),
    ("grep_file", f"grep a smoke/d.txt {R}", "a\n"),
    ("grep_c_file", f"grep -c a smoke/d.txt {R}", "1\n"),
    ("grep_stdin", f"cat smoke/d.txt | grep a {R}", "a\n"),
    ("grep_v", f"grep -v a smoke/d.txt {R}", "b\nc\n"),
    ("grep_r", f"grep -rl needle smoke/sub {R}", "smoke/sub/n.txt\n"),
    ("find_name", f"find smoke -name d.txt {R}", "smoke/d.txt\n"),
    ("ls_one", f"ls smoke/sub2 {R}", "x\n"),
    ("ls_l_ok", f"ls -l smoke/sub2 | wc -l {R}", "2\n"),
    ("cp_ok", f"cp smoke/d.txt smoke/d2.txt; cat smoke/d2.txt {R}", "b\na\nc\n"),
    ("mv_ok", f"mv smoke/d2.txt smoke/d3.txt; cat smoke/d3.txt {R}", "b\na\nc\n"),
    ("rm_ok", f"rm smoke/d3.txt; ls smoke/d3.txt {R}", "ls: cannot access 'smoke/d3.txt': No such file or directory\n"),
    ("mkdir_ok", f"mkdir -p smoke/m/n; ls smoke/m {R}", "n\n"),
    ("touch_new", f"touch smoke/m/t; ls smoke/m/t {R}", "smoke/m/t\n"),
    ("touch_err", f"touch smoke/m/t2 {R}", ""),
    ("basename", f"basename smoke/d.txt {R}", "d.txt\n"),
    ("dirname", f"dirname smoke/d.txt {R}", "smoke\n"),
    ("seq", f"seq 1 3 {R}", "1\n2\n3\n"),
    ("date_y", f"date +%Y {R}", time.strftime("%Y") + "\n"),
    ("stat_size", f"stat -c %s smoke/d.txt {R}", "6\n"),
    ("du_ok", f"du -s smoke/sub2 | wc -l {R}", "1\n"),
    ("xargs", f"echo smoke/d.txt | xargs cat {R}", "b\na\nc\n"),
    ("xargs_a", f"xargs -a smoke/d.txt echo {R}", "b a c\n"),
    ("tee_ok", f"echo t | tee smoke/tee.txt > /dev/null; cat smoke/tee.txt {R}", "t\n"),
    ("gzip_rt", f"cp smoke/d.txt smoke/g.txt; gzip -f smoke/g.txt; gunzip -f smoke/g.txt.gz; cat smoke/g.txt {R}", "b\na\nc\n"),
    ("tar_rt", f"tar cf smoke/x.tar -C smoke sub2; tar tf smoke/x.tar | head -1 {R}", "sub2/\n"),
    ("base64", f"echo hi | base64 {R}", "aGkK\n"),
    ("md5sum", f"md5sum smoke/d.txt | wc -l {R}", "1\n"),
    ("tac_file", f"tac smoke/d.txt {R}", "c\na\nb\n"),
    ("diff_same", f"diff smoke/d.txt smoke/d.txt {R}", ""),
    ("diff_differ", f"diff smoke/d.txt smoke/u.txt | wc -l {R}", "7\n"),
    ("cmp_same", f"cmp smoke/d.txt smoke/d.txt {R}", ""),
    ("split_ok", f"split -l 1 smoke/d.txt smoke/sp. > smoke/res/split_err.txt 2>&1; echo rc=$? >> smoke/res/split_err.txt; ls -l smoke/sp.* >> smoke/res/split_err.txt 2>&1; cat smoke/sp.aa {R}", "b\n"),
    ("nl_file", f"nl smoke/d.txt | wc -l {R}", "3\n"),
    ("od_file", f"od -c smoke/c.txt | head -1 | wc -l {R}", "1\n"),
    ("expr_sh", f"echo $((6*7)) {R}", "42\n"),
    ("subshell", f"echo $(echo hi) {R}", "hi\n"),
    ("cmdsub_tool", f"echo $(cat smoke/d.txt | head -1) {R}", "b\n"),
    ("append_echo", f"echo r {R}; echo s {A}", "r\ns\n"),
    ("for_loop", f"for i in 1 2; do echo $i {A}; done", "1\n2\n"),
    ("pipe_three", f"printf 'b\\na\\n' | sort | head -1 {R}", "a\n"),
    ("brace_redirect", "{ echo b; } > {o} 2>&1", "b\n"),
    ("sub_redirect", "( echo s ) > {o} 2>&1", "s\n"),
    # Compound commands run apart from the shell: bash_parity_subshell and friends.
    ("subshell_exit", f"( exit 3 ); echo \"after $?\" {R}", "after 3\n"),
    ("subshell_var", f"X=1; ( X=2 ); echo \"X=$X\" {R}", "X=1\n"),
    ("subshell_cd", f"d=$(pwd); ( cd smoke ); [ \"$(pwd)\" = \"$d\" ] && echo same {R}", "same\n"),
    ("subshell_cd_inside", f"( cd smoke; cat d.txt ) | head -1 {R}", "b\n"),
    ("subshell_status", f"( false ); echo \"status $?\" {R}", "status 1\n"),
    ("subshell_export", f"( export SE=1; env | grep -c '^SE=1$' ) {R}", "1\n"),
    ("subshell_export_gone", f"echo \"[$SE]\" {R}", "[]\n"),
    ("subshell_function", f"( fn_in() {{ echo in; }}; fn_in ) {R}; fn_in {A}", None),
    ("subshell_nested", f"( ( exit 8 ); echo \"nested $?\" ) {R}", "nested 8\n"),
    ("subshell_applet", f"( cat smoke/d.txt; ls smoke/d.txt ) | wc -l {R}", None),
    ("group_pipe", f"{{ echo c; echo d; }} | cat {R}", "c\nd\n"),
    ("subshell_pipe", f"( echo e; echo f ) | cat {R}", "e\nf\n"),
    # What bash_parity_* covers on hosts, here where stages are threads: parity round three.
    ("parity_big_substitution", f"x=$(seq 1 30000); echo ${{#x}} {R}", "168893\n"),
    ("parity_array_quoting", f"a=(\"b c\" d); echo \"${{#a[@]}} ${{a[0]}}\" {R}", "2 b c\n"),
    ("parity_assoc_array", f"declare -A pm; pm[k]=v; echo \"${{pm[k]}}\" {R}", "v\n"),
    ("parity_tilde", f"[ ~ = \"$HOME\" ] && echo same {R}", "same\n"),
    ("parity_replace", f"x=abcb; echo ${{x/b/B}} ${{x//b/B}} {R}", "aBcb aBcB\n"),
    ("parity_regex", f"[[ abc =~ ^(a)b ]] && echo \"${{BASH_REMATCH[1]}}\" {R}", "a\n"),
    ("parity_bracket_and", f"[[ a == a && b == b ]] && echo y {R}", "y\n"),
    ("parity_brace", f"echo {{1..3}} x{{a,b}} {R}", "1 2 3 xa xb\n"),
    ("parity_printf_escapes", f"printf 'a\\101b %b\\n' 'c\\tc' {R}", "aAb c\tc\n"),
    ("parity_ansi_c", f"echo $'a\\tb' {R}", "a\tb\n"),
    ("parity_ifs_join", f"( set -- a b; IFS=,; echo \"$*\" ) {R}", "a,b\n"),
    ("parity_pipestatus", f"false | true; echo \"${{PIPESTATUS[@]}}\" {R}", "1 0\n"),
    ("parity_arithmetic", f"echo $((7%3)) $((2**10)) $((1?2:3)) {R}", "1 1024 2\n"),
    ("parity_function_stage", f"pf() {{ tr a b; }}; echo a | pf {R}", "b\n"),
    ("parity_eval_stage", f"echo a | eval 'tr a b' {R}", "b\n"),
    ("parity_function_head", f"pg() {{ cat smoke/d.txt; }}; pg | head -1 {R}", "b\n"),
    ("parity_function_stage_var", f"pv() {{ PV=in; }}; PV=out; echo | pv; echo \"$PV\" {R}", "out\n"),
    ("parity_process_subst", f"wc -l < <(seq 1 3) | tr -d ' ' {R}", "3\n"),
    ("parity_stage_stdio_reuse",
     f"{{ for i in 1 2 3 4 5 6 7 8; do ls /nonexistent/zz 2>&1 | wc -l | tr -d ' '; done; }} {R}", "1\n" * 8),
    ("parity_here_string_fd", f"exec 3<<< from3; read -u 3 pr; exec 3<&-; echo \"$pr\" {R}", "from3\n"),
    ("parity_noclobber", f"rm -f smoke/nc; ( set -C; echo a > smoke/nc; echo \"st $?\" ) {R}; rm -f smoke/nc", "st 0\n"),
    ("parity_dotglob", f"( mkdir smoke/dg; cd smoke/dg; touch .a b; shopt -s dotglob; echo * ) {R}; rm -r smoke/dg", ".a b\n"),
    ("pipe_into_group", f"echo o | {{ read x; echo \"got=$x\"; }} {R}", "got=o\n"),
    ("pipe_into_subshell", f"echo o | ( read x; echo \"got=$x\" ) {R}", "got=o\n"),
    ("group_both_ends", f"{{ echo 1; echo 2; }} | {{ read a; read b; echo \"$b$a\"; }} {R}", "21\n"),
    ("pipe_var_gone", f"V=1; echo x | {{ read V; }}; echo \"V=$V\" {R}", "V=1\n"),
    ("if_pipe", f"if true; then echo t1; echo t2; fi | cat {R}", "t1\nt2\n"),
    ("case_pipe", f"case x in x) echo c1; echo c2;; esac | cat {R}", "c1\nc2\n"),
    ("for_pipe", f"for i in 1 2 3; do echo $i; done | cat {R}", "1\n2\n3\n"),
    ("while_read_pipe", f"printf 'l1\\nl2\\n' | while read l; do echo \"[$l]\"; done {R}", "[l1]\n[l2]\n"),
    ("group_inner_pipe", f"{{ printf 'b\\na\\n' | sort; echo z; }} | head -2 {R}", "a\nb\n"),
    ("group_merge_stderr", f"{{ echo o; }} |& cat {R}", "o\n"),
    ("group_large_output", f"{{ seq 1 50000; }} | wc -l | tr -d ' ' {R}", "50000\n"),
    ("loop_large_output", f"i=0; while [ $i -lt 5000 ]; do echo \"line $i\"; i=$((i+1)); done | wc -l | tr -d ' ' {R}", "5000\n"),
    ("group_closed_reader", f"{{ while :; do echo y; done; }} | head -1 {R}", "y\n"),
    ("group_background", f"{{ sleep 1; echo bg {A}; }} & echo first {R}; wait", "first\nbg\n"),
    ("subshell_background", f"( sleep 1; echo bg {A} ) & echo first {R}; wait", "first\nbg\n"),
    ("and_list_background", f"sleep 1 && echo bg {A} & echo first {R}; wait", "first\nbg\n"),
    ("pipeline_background", f"{{ sleep 1; echo bg; }} | cat {A} & echo first {R}; wait", "first\nbg\n"),
    ("background_var", f"B=1; {{ B=2; sleep 1; echo \"group $B\" {A}; }} & echo \"shell $B\" {R}; wait", "shell 1\ngroup 2\n"),
    # A job with its output redirected lets go of the pipe it replaced: the reader sees end of
    # file while the job runs. The job marks a held pipe and gives up, so this cannot hang.
    ("background_release", f"( {{ : > smoke/rel.up; i=0; while [ ! -e smoke/rel.seen ] && [ $i -lt 30 ]; do i=$((i+1)); sleep 0.1; done; [ -e smoke/rel.seen ] || : > smoke/rel.held; }} > /dev/null 2>&1 & until [ -e smoke/rel.up ]; do sleep 0.05; done ) | {{ cat > /dev/null; if [ -e smoke/rel.held ]; then echo held; else echo released; fi; : > smoke/rel.seen; }} {R}; wait", "released\n"),
    ("subshell_exit_trap_redirect", f"( trap 'echo bye' EXIT; echo body ) > smoke/et.txt; cat smoke/et.txt {R}", "body\nbye\n"),
    ("stage_redirect_undone", f"echo a | {{ cat; echo b; }} > smoke/st.txt; echo back {R}; cat smoke/st.txt {A}", "back\na\nb\n"),
    ("background_status", f"{{ exit 7; }} & wait $!; echo \"wait $?\" {R}", "wait 7\n"),
    ("background_status_again", f"( exit 8 ) & wait $!; echo \"wait $?\" {R}", "wait 8\n"),
    ("background_status_late", f"{{ exit 9; }} & p=$!; sleep 1; wait $p; echo \"wait $?\" {R}", "wait 9\n"),
    ("background_simple_status", f"false & wait $!; echo \"wait $?\" {R}", "wait 1\n"),
    ("background_nested_status", f"sh -c '{{ exit 6; }} & wait $!; echo \"wait $?\"' {R}", "wait 6\n"),
    ("subshell_in_sh", f"sh -c '( exit 3 ); echo \"in sh $?\"' {R}", "in sh 3\n"),
    ("group_in_sh", f"sh -c '{{ echo a; echo b; }} | cat' {R}", "a\nb\n"),
    ("frontend_in_group", f"{{ pascal smoke/hello.pas; clike smoke/hello.clike; }} | cat {R}", "pascal ok 42\nclike ok 42\n"),
    ("frontend_in_subshell", f"( pascal smoke/hello.pas ) {R}", "pascal ok 42\n"),
    ("frontend_into_group", f"pascal smoke/hello.pas | {{ read x; echo \"got $x\"; }} {R}", "got pascal ok 42\n"),
    ("aether_rea_in_group", f"{{ aether smoke/hello.aether; rea smoke/hello.rea; }} | cat {R}", "aether ok 42\nrea ok 42\n"),
    ("function_in_group", f"f2() {{ echo \"f2 $1\"; }}; {{ f2 a; f2 b; }} | cat {R}", "f2 a\nf2 b\n"),
    ("subshell_in_function", f"f3() {{ ( echo \"$1 in sub\" ); }}; f3 z {R}", "z in sub\n"),
    ("subshell_params", f"set -- p1 p2; ( echo \"$1 $#\"; shift; echo \"$1 $#\" ) {R}; echo \"$1 $#\" {A}; set --", "p1 2\np2 1\np1 2\n"),
    ("subshell_exit_trap", f"( trap 'echo sub-exit {A}' EXIT; echo body {R} )", "body\nsub-exit\n"),
    ("exsh_c", f"exsh -c 'echo inner' {R}", "inner\n"),
    ("exsh_script", f"exsh smoke/s.sh {R}", "script\n"),
    ("sh_script", f"sh smoke/s.sh {R}", "script\n"),
    ("sh_c", f"sh -c 'echo inner | cat' {R}", "inner\n"),
    ("md_view", f"printf '# t\\n' | md 2>&1 | wc -l {R}", None),
    ("env_var", f"X=1 env | grep -c '^X=1$' {R}", "1\n"),
    ("pascal_run", f"pascal smoke/hello.pas {R}", "pascal ok 42\n"),
    ("clike_run", f"clike smoke/hello.clike {R}", "clike ok 42\n"),
    # Build 31's cache writer took a slot for a host descriptor. Where the bytes went depends on
    # which descriptors are open, but its entries were never bytecode, so this always shows it.
    ("clike_cache_entry", f'clike smoke/hello.clike > /dev/null 2>&1; cat "$HOME"/.pscal/bc_cache/clike-hello.clike-* | head -c 4 {R}', "3BSP"),
    ("aether_run", f"aether smoke/hello.aether {R}", "aether ok 42\n"),
    ("rea_after_aether", f"rea smoke/hello.rea {R}", "rea ok 42\n"),
    ("aether_after_rea", f"aether smoke/hello.aether {R}", "aether ok 42\n"),
    ("rea_again", f"rea smoke/hello.rea {R}", "rea ok 42\n"),
    ("pascal_pipe", f"pascal smoke/hello.pas | cat {R}", "pascal ok 42\n"),
    ("clike_pipe", f"clike smoke/hello.clike | cat {R}", "clike ok 42\n"),
    ("aether_pipe", f"aether smoke/hello.aether | cat {R}", "aether ok 42\n"),
    ("rea_pipe", f"rea smoke/hello.rea | cat {R}", "rea ok 42\n"),
    ("order_seq", f"echo one {R}; pascal smoke/hello.pas {A}; echo two {A}; clike smoke/hello.clike {A}; echo three {A}; aether smoke/hello.aether {A}; echo four {A}", "one\npascal ok 42\ntwo\nclike ok 42\nthree\naether ok 42\nfour\n"),
    ("pascal_rc", f"pascal smoke/hello.pas > /dev/null 2>&1; echo $? {R}", "0\n"),
    ("bad_rc", f"grep -q zzz smoke/d.txt; echo $? {R}", "1\n"),
    # Each of these runs on a worker thread and hands its status back through wait.
    ("pascal_fail_rc", f"pascal smoke/nope.pas > /dev/null 2>&1; echo $? {R}", "1\n"),
    ("clike_fail_rc", f"clike smoke/nope.clike > /dev/null 2>&1; echo $? {R}", "1\n"),
    ("rea_fail_rc", f"rea smoke/nope.rea > /dev/null 2>&1; echo $? {R}", "1\n"),
    ("aether_fail_rc", f"aether smoke/nope.aether > /dev/null 2>&1; echo $? {R}", "1\n"),
    ("pascal_fail_or", f"pascal smoke/nope.pas 2>/dev/null || echo failed {R}", "failed\n"),
    ("pascal_halt_rc", f"pascal smoke/halt.pas > /dev/null 2>&1; echo $? {R}", "3\n"),
    ("applet_fail_rc", f"awk 1 smoke/nope > /dev/null 2>&1; echo $? {R}", "2\n"),
    ("shebang_fail_rc", f"smoke/sb3.sh > /dev/null 2>&1; echo $? {R}", "3\n"),
    # A #! frontend has no process exit to flush its output: it arrived after the
    # redirection was gone, or never.
    ("shebang_frontend_file", f"Examples/clike/base/hello {R}", "Hello World!\n"),
    ("shebang_frontend_sub", f"x=$(Examples/clike/base/hello); echo \"got $x\" {R}", "got Hello World!\n"),
    ("shebang_frontend_pipe", f"Examples/clike/base/hello | cat {R}", "Hello World!\n"),
    ("shebang_frontend_time", f"time Examples/clike/base/hello | head -1 {R}", "Hello World!\n"),
    # Slots used as host descriptors: fchdir left find in the first directory, and
    # fopen on a device read whatever host descriptor had the slot's number.
    ("find_execdir", f"find smoke/sub smoke/sub2 -type f -execdir pwd \\; {R}", "/home/smoke/sub\n/home/smoke/sub2\n"),
    ("wc_dev_tty", f"wc -c /dev/tty3 {R}", "0 /dev/tty3\n"),
    # fcntl, pread and pwrite had no shim: utmp records were read from and written to
    # whichever host descriptor had the slot's number, or nowhere if none did.
    ("who_file", f"who -q smoke/utmp.rec {R}", "alice bob\n# users=2\n"),
    ("users_file", f"users smoke/utmp.rec {R}", "alice bob\n"),
    ("uptime_users", f"mkdir -p /var/run; cp smoke/utmp.rec /var/run/utmp; uptime | grep -c ' 2 users, ' {R}; rm /var/run/utmp", "1\n"),
    # xargs, because at the prompt halt and poweroff are the VM's Halt builtin.
    ("wtmp_append", f"mkdir -p /var/log; rm -f /var/log/wtmp; xargs halt -w < /dev/null {R}; xargs halt -w < /dev/null {A}; wc -c < /var/log/wtmp {A}; who -r /var/log/wtmp | wc -l {A}", "800\n2\n"),
    ("wtmp_partial", f"printf x > /var/log/wtmp; xargs halt -w < /dev/null {R}; wc -c < /var/log/wtmp {A}", "400\n"),
    # And F_SETFL reached a host descriptor too: fd 0 from tail, a system directory
    # from stty. Only a trace shows that, so these say the paths still work; the shims
    # have their tests in Tests/ios_vproc.
    ("tail_f_pipe", f"cat smoke/d.txt | tail -f --pid=999999 {R}", "b\na\nc\n"),
    ("tail_f_stdin", f"tail -f --pid=999999 < /dev/null {R}; echo $? {A}", "0\n"),
    ("tail_f_device", f"tail -f --pid=999999 /dev/null {R}; echo $? {A}", "0\n"),
    ("tail_F_appears", f"tail -F -s 0.2 smoke/appear {R} & t=$!; sleep 0.5; ln -s /dev/null smoke/appear; i=0; while [ $i -lt 200 ] && ! grep -q appeared {{o}}; do sleep 0.1; i=$((i+1)); done; kill $t; wait $t 2>/dev/null", "tail: cannot open 'smoke/appear' for reading: No such file or directory\ntail: 'smoke/appear' has appeared;  following new file\n"),
    ("stty_named", f"stty -F /dev/tty size > /dev/null 2>&1; echo $? {R}", "0\n"),
    ("stty_not_tty", f"stty -F /dev/null {R}; echo $? {A}", "stty: /dev/null: Inappropriate ioctl for device\n1\n"),
    # A missing directory is an error, as in bash: nothing makes it on the way.
    ("redirect_no_dir", f"echo x > /nonexistent/dir/f; echo $? {R}; ls /nonexistent {A}", "1\nls: cannot access '/nonexistent': No such file or directory\n"),
    ("touch_no_dir", f"touch /nonexistent/dir/f > /dev/null 2>&1; echo $? {R}; ls /nonexistent {A}", "1\nls: cannot access '/nonexistent': No such file or directory\n"),
    ("cp_no_dir", f"cp smoke/d.txt /nonexistent/dir/f > /dev/null 2>&1; echo $? {R}; ls /nonexistent {A}", "1\nls: cannot access '/nonexistent': No such file or directory\n"),
    ("tr_cat", f"cat smoke/d.txt | tr a-z A-Z {R}", "B\nA\nC\n"),
    ("awk_cat", f"cat smoke/d.txt | awk '{{print $1}}' {R}", "b\na\nc\n"),
    ("awk_lt", f"awk '{{print $1}}' < smoke/d.txt {R}", "b\na\nc\n"),
    ("b64_cat", f"cat smoke/c.txt | base64 {R}", "b25lOnR3bwo=\n"),
    ("od_cat", f"cat smoke/c.txt | od -c | wc -l {R}", "2\n"),
    ("rev_cat", f"cat smoke/c.txt | rev {R}", "owt:eno\n"),
    ("uniq_cat", f"cat smoke/u.txt | uniq {R}", "a\nb\n"),
    ("nl_cat", f"cat smoke/d.txt | nl | wc -l {R}", "3\n"),
    ("fold_cat", f"cat smoke/c.txt | fold {R}", "one:two\n"),
    ("tac_cat", f"cat smoke/d.txt | tac {R}", "c\na\nb\n"),
    ("md5_cat", f"cat smoke/c.txt | md5sum | wc -l {R}", "1\n"),
    ("sum_cat", f"cat smoke/c.txt | sum | wc -l {R}", "1\n"),
    ("paste_cat", f"cat smoke/d.txt | paste - {R}", "b\na\nc\n"),
    ("tr_twice", f"echo a | tr a b {R}; echo c | tr c d {A}", "b\nd\n"),
    ("script_order", f"exsh smoke/ord.sh", "slow done\nafter-pascal\nclike ok 42\nafter-clike\n"),
    ("shebang_order", f"smoke/ord_sb.sh", "slow done\nafter-pascal\n"),
    ("source_order", f". smoke/ord_src.sh", "slow done\nafter-pascal\n"),
    ("shebang_pipe", f"smoke/sb.sh a | cat {R}", "sb 1\n"),
    ("shebang_pipe2", f"smoke/sb.sh a b | cat {R}", "sb 2\n"),
    ("shebang_set", f"smoke/sbset.sh a {R}", "x y\n"),
    ("shebang_set_pipe", f"smoke/sbset.sh a | cat {R}", "x y\n"),
    ("shebang_pipe_loop", f"for i in 1 2 3 4 5 6 7 8; do smoke/sb.sh a | cat {A}; done", "sb 1\nsb 1\nsb 1\nsb 1\nsb 1\nsb 1\nsb 1\nsb 1\n"),
    ("ln_s", f"ln -sf d.txt smoke/lnk; cat smoke/lnk {R}", "b\na\nc\n"),
    ("rmdir_ok", f"mkdir smoke/rd; rmdir smoke/rd; ls smoke/rd {R}", "ls: cannot access 'smoke/rd': No such file or directory\n"),
    ("touch_times", f"touch -t 202001020304 smoke/tt.txt; date -r smoke/tt.txt +%Y {R}", "2020\n"),
    ("gzip_keep_time", f"cp smoke/d.txt smoke/g2.txt; gzip -f smoke/g2.txt; zcat smoke/g2.txt.gz {R}", "b\na\nc\n"),
    # A compiler prints nothing here. Where this file got host descriptor 3, build 31 captured
    # all of tiny.clike's cache entry, about 21 KB: its cache writer took slot 3 for that descriptor.
    ("tiny_compile", f"tiny smoke/max.tiny smoke/max.pbc {R}", ""),
    ("tiny_pipe", f"tiny smoke/max.tiny smoke/max2.pbc | cat {R}", ""),
    ("word_home", f"Q=abc; echo $Q/foo {R}", "abc/foo\n"),
    ("word_brace", f"Q=abc; echo ${{Q}}x {R}", "abcx\n"),
    ("word_two", f"Q=abc; echo $Q$Q {R}", "abcabc\n"),
    ("word_cat_home", f"cat $HOME/smoke/d.txt {R}", "b\na\nc\n"),
    ("pos_script", f"smoke/pos.sh a b {R}", "a b\n"),
    ("pos_brace", f"smoke/posb.sh a b {R}", "a/b\n"),
    ("pos_word", f"smoke/posw.sh a {R}", "a.txt\n"),
    ("tiny_run_out", f"tiny --run smoke/p.tiny smoke/p.pbc | cat {R}", "42\n"),
    ("tiny_run_default", f"tiny --run smoke/p.tiny | cat {R}", "42\n"),
    ("ls_home_abs", f"ls /home/smoke/sub2 {R}", "x\n"),
    ("ls_root_view", f"ls / | grep -c '^Applications$' {R}", "0\n"),
    ("ls_bin", f"ls /bin | grep -c '^tiny$' {R}", "1\n"),
    ("mkdir_abs", f"mkdir -p /home/smoke/ma/mb; ls /home/smoke/ma {R}", "mb\n"),
    ("touch_abs", f"touch /home/smoke/ma/t; ls /home/smoke/ma/t {R}", "/home/smoke/ma/t\n"),
    ("cp_abs", f"cp /home/smoke/d.txt /home/smoke/ma/d; cat /home/smoke/ma/d {R}", "b\na\nc\n"),
    ("mv_abs", f"mv /home/smoke/ma/d /home/smoke/ma/e; cat /home/smoke/ma/e {R}", "b\na\nc\n"),
    ("rm_abs", f"rm /home/smoke/ma/e; ls /home/smoke/ma {R}", "mb\nt\n"),
    ("rm_r_abs", f"rm -r /home/smoke/ma; ls /home/smoke/ma {R}", "ls: cannot access '/home/smoke/ma': No such file or directory\n"),
    ("empty_path_rm", f"touch /zz_em; mkdir smoke/em; touch smoke/em/zz_em; ( cd smoke/em; rm -rf \"\"; echo \"rf $?\"; rm -rf \"$nosuch\"; echo \"unset $?\"; rm -r \"\"; echo \"r $?\"; ln -s zz_ln \"\"; echo \"ln $?\" ) {R}; ls /zz_em smoke/em/zz_em {A}; [ -L /zz_ln ]; echo \"link $?\" {A}; rm -rf /zz_em /zz_ln smoke/em", "rf 0\nunset 0\nrm: cannot remove '': No such file or directory\nr 1\nln: failed to create symbolic link '': No such file or directory\nln 1\n/zz_em\nsmoke/em/zz_em\nlink 1\n"),
    ("empty_path_other", f"ls \"\" {R}; echo \"ls $?\" {A}; cat \"\" {A}; echo \"cat $?\" {A}; cd \"\" {A}; echo \"cd $?\" {A}", "ls: cannot access '': No such file or directory\nls 2\ncat: '': No such file or directory\ncat 1\ncd 0\n"),
    ("find_abs", f"find /home/smoke/sub2 -type f {R}", "/home/smoke/sub2/x\n"),
    ("du_abs", f"du -s /home/smoke/sub2 | wc -l {R}", "1\n"),
    ("stat_abs", f"stat -c %s /home/smoke/d.txt {R}", "6\n"),
    ("sed_abs", f"sed s/a/z/ /home/smoke/d.txt {R}", "b\nz\nc\n"),
    ("grep_abs", f"grep a /home/smoke/d.txt {R}", "a\n"),
    ("tar_abs", f"tar cf /home/smoke/y.tar -C /home/smoke sub2; tar tf /home/smoke/y.tar | head -1 {R}", "sub2/\n"),
    ("realpath_v", f"realpath /home/smoke/d.txt {R}", "/home/smoke/d.txt\n"),
    ("tiny_full", f"tiny smoke/cd.tiny /home/smoke/cd3.pbc {R}; ls smoke/cd3.pbc {A}", "smoke/cd3.pbc\n"),
    # cd in a script reaches what the script runs, and a shell run as a command leaves its
    # caller where it was. These ran in the old directory, with no error: the same name in
    # both directories shows which copy was hit, and `rm` took the wrong one.
    ("cd_source_rm", f"printf 'H\\n' > cdv; mkdir smoke/cd1; printf 'D\\n' > smoke/cd1/cdv; printf 'cd smoke/cd1\\nrm cdv\\ncd \"$HOME\"\\n' > smoke/c1.sh; . smoke/c1.sh; cat cdv {R}; ls smoke/cd1 {A}; rm -f cdv", "H\n"),
    ("cd_source_mv", f"printf 'H\\n' > cdv; mkdir smoke/cd2; printf 'D\\n' > smoke/cd2/cdv; printf 'cd smoke/cd2\\nmv cdv cdw\\ncd \"$HOME\"\\n' > smoke/c2.sh; . smoke/c2.sh; cat cdv {R}; ls smoke/cd2 {A}; rm -f cdv cdw", "H\ncdw\n"),
    ("cd_source_redirect", f"printf 'cd smoke/sub\\necho there > r.txt\\ncat n.txt > r2.txt\\ncd \"$HOME\"\\n' > smoke/c3.sh; . smoke/c3.sh; cat smoke/sub/r.txt smoke/sub/r2.txt {R}", "there\nneedle\n"),
    ("cd_source_stays", f"printf 'cd smoke/sub\\n' > smoke/c4.sh; . smoke/c4.sh; pwd > \"$HOME/smoke/res/cd_source_stays.txt\"; cat n.txt >> \"$HOME/smoke/res/cd_source_stays.txt\"; cd \"$HOME\"; pwd {A}", "/home/smoke/sub\nneedle\n/home\n"),
    ("cd_source_frontend", f"printf 'cd smoke/sub\\npascal ../hello.pas > f.txt 2>&1\\nclike ../hello.clike >> f.txt 2>&1\\ncd \"$HOME\"\\n' > smoke/c5.sh; . smoke/c5.sh; cat smoke/sub/f.txt {R}", "pascal ok 42\nclike ok 42\n"),
    ("cd_two_levels", f"printf '. smoke/c7.sh\\ncat n.txt\\ncd \"$HOME\"\\n' > smoke/c6.sh; printf 'cd smoke/sub\\n' > smoke/c7.sh; . smoke/c6.sh {R}", "needle\n"),
    ("cd_eval", f"eval 'cd smoke/sub; cat n.txt; cd \"$HOME\"' {R}", "needle\n"),
    ("cd_exsh_rm", f"printf 'H\\n' > cdv; mkdir smoke/cd3; printf 'D\\n' > smoke/cd3/cdv; printf 'cd smoke/cd3\\nrm cdv\\n' > smoke/c8.sh; exsh smoke/c8.sh; cat cdv {R}; ls smoke/cd3 {A}; pwd {A}; rm -f cdv", "H\n/home\n"),
    ("cd_exsh_keeps_caller", f"printf 'cd smoke/sub\\ncat n.txt\\npwd\\n' > smoke/c9.sh; exsh smoke/c9.sh {R}; pwd {A}; echo \"$PWD\" {A}", "needle\n/home/smoke/sub\n/home\n/home\n"),
    ("cd_sh_c", f"sh -c 'cd smoke/sub; cat n.txt; pwd' {R}; pwd {A}", "needle\n/home/smoke/sub\n/home\n"),
    ("cd_shebang_rm", f"printf 'H\\n' > cdv; mkdir smoke/cd4; printf 'D\\n' > smoke/cd4/cdv; printf '#!/bin/exsh\\ncd smoke/cd4\\nrm cdv\\n' > smoke/c10.sh; chmod +x smoke/c10.sh; smoke/c10.sh; cat cdv {R}; ls smoke/cd4 {A}; pwd {A}; rm -f cdv", "H\n/home\n"),
    ("cd_sub_keeps_caller", f"x=$(cd smoke/sub; cat n.txt; pwd); echo \"$x\" {R}; pwd {A}", "needle\n/home/smoke/sub\n/home\n"),
    ("cd_nested_shells", f"sh -c 'cd smoke; sh -c \"cd sub; pwd\"; pwd' {R}; pwd {A}", "/home/smoke/sub\n/home/smoke\n/home\n"),
    # A descriptor above stdio is the vproc's slot. A host dup2 onto that number ended the
    # whole app when it held the Go runtime's kqueue or its poller's pipe.
    ("fd3_write", f"echo x 3> smoke/f3; echo survived {R}", "survived\n"),
    ("fd5_write", f"echo x 5> smoke/f5; echo survived {R}", "survived\n"),
    ("fd3_applet", f"ls smoke/d.txt 3> smoke/f3 {R}", "smoke/d.txt\n"),
    ("fd3_group", f"{{ echo in-group; }} 3> smoke/f3 {R}", "in-group\n"),
    ("fd3_frontend", f"pascal smoke/hello.pas 3> smoke/f3 {R}", "pascal ok 42\n"),
    ("fd3_uses", f"{{ echo to-three >&3; }} 3> smoke/f3b; cat smoke/f3b {R}", "to-three\n"),
    ("exec3_write", f"exec 3> smoke/e3; echo hi >&3; exec 3>&-; cat smoke/e3 {R}", "hi\n"),
    ("exec5_write", f"exec 5> smoke/e5; echo hi5 >&5; exec 5>&-; cat smoke/e5 {R}", "hi5\n"),
    ("exec4_applet", f"exec 4> smoke/e4; ls smoke/d.txt >&4; exec 4>&-; cat smoke/e4 {R}", "smoke/d.txt\n"),
    ("exec3_read", f"exec 3< smoke/d.txt; read l <&3; exec 3<&-; echo \"$l\" {R}", "b\n"),
    ("exec6_dup", f"exec 6>&1; echo to-six >&6 {R}; exec 6>&-", "to-six\n"),
    ("exec3_gone", f"exec 3> smoke/e6; exec 3>&-; echo x 2>/dev/null >&3; echo \"rc $?\" {R}", "rc 1\n"),
    ("fd_unopened", f"echo x 2>/dev/null >&7; echo \"rc $?\" {R}", "rc 1\n"),
    ("exec_in_source", f"printf 'exec 8> smoke/e8\\necho in-source >&8\\n' > smoke/c11.sh; . smoke/c11.sh; echo after-source >&8; exec 8>&-; cat smoke/e8 {R}", "in-source\nafter-source\n"),
    # A substitution reads its pipe while the command runs. It read only after the command
    # ended, so a command that wrote more than a pipe holds never ended. Each case unsets
    # what it captured: every variable reaches the process environment.
    ("sub_big_applet", f'x=$(seq 1 30000); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_backtick", f'x=`seq 1 30000`; echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_last", f'echo "$(seq 1 30000)" | tail -1 {R}', "30000\n"),
    ("sub_big_builtins", f'x=$(i=0; while [ "$i" -lt 8000 ]; do echo "line number $i"; i=$((i+1)); done); echo "${{#x}}" {R}; unset x', "134889\n"),
    ("sub_big_echo", f'x=$(seq 1 30000); y=$(echo "$x"; echo "$x"); echo "${{#y}}" {R}; unset x y', "337787\n"),
    ("sub_big_pipeline", f'x=$(seq 1 30000 | cat); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_pipeline3", f'x=$(seq 1 30000 | cat | cat); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_group", f'x=$({{ seq 1 30000; echo end; }}); echo "${{#x}}" {R}; unset x', "168897\n"),
    ("sub_big_subshell", f'x=$( (seq 1 30000) ); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_loop", f'x=$(for i in 1 2; do seq 1 30000; done); echo "${{#x}}" {R}; unset x', "337787\n"),
    ("sub_big_function", f'bigf() {{ seq 1 30000; }}; x=$(bigf); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_nested", f'x=$(echo "$(seq 1 30000)"); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_file", f'seq 1 30000 > smoke/big.txt; x=$(cat smoke/big.txt); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_stdin", f'x=$(cat < smoke/big.txt); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_status", f'x=$(seq 1 30000; exit 3); echo "$? ${{#x}}" {R}; unset x', "3 168893\n"),
    ("sub_big_pascal", f'x=$(pascal smoke/bigout.pas); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_clike", f'x=$(clike smoke/bigout.clike); echo "${{#x}}" {R}; unset x', "168893\n"),
    ("sub_big_sh_c", f"x=$(sh -c 'seq 1 30000'); echo \"${{#x}}\" {R}; unset x", "168893\n"),
    ("sub_big_in_sh_c", f"sh -c 'x=$(seq 1 30000); echo ${{#x}}' {R}", "168893\n"),
    ("sub_big_script", f"printf 'x=$(seq 1 30000)\\necho ${{#x}}\\n' > smoke/bs.sh; exsh smoke/bs.sh {R}", "168893\n"),
    ("sub_big_shebang", f"printf '#!/bin/exsh\\nx=$(seq 1 30000)\\necho ${{#x}} {R}\\n' > smoke/bs2.sh; chmod +x smoke/bs2.sh; smoke/bs2.sh", "168893\n"),
    ("sub_big_in_subshell", f'( x=$(seq 1 30000); echo "${{#x}}" {R} )', "168893\n"),
    ("sub_big_in_stage", f'echo a | {{ x=$(seq 1 30000); echo "${{#x}}"; }} {R}', "168893\n"),
    ("sub_big_background", f'( x=$(seq 1 30000); echo "${{#x}}" > smoke/bgb.txt ) & wait $!; cat smoke/bgb.txt {R}', "168893\n"),
    ("sub_big_twenty", f'n=0; s=0; while [ "$n" -lt 20 ]; do x=$(seq 1 30000); s=$((s+${{#x}})); n=$((n+1)); done; unset x; echo "$s" {R}', "3377860\n"),
    ("sub_many_small", f'n=0; while [ "$n" -lt 300 ]; do v=$(echo "$n"); n=$((n+1)); done; echo "$v" {R}', "299\n"),
    ("sub_fd3_exec", f'x=$(exec 3> smoke/s3; echo out); echo "[$x]" {R}; unset x', "[out]\n"),
    ("sub_fd3_cmd", f'x=$(echo hi 3> smoke/s3); echo "[$x]" {R}; unset x', "[hi]\n"),
    ("sub_bg_holds", f"printf 'x=$(sleep 1 & echo bg)\\necho \"[$x]\"\\n' > smoke/bh.sh; exsh smoke/bh.sh {R}", "[bg]\n"),
    # Killing a body that is inside a substitution leaves that substitution's reader with no
    # one to join it. The next substitution still has to work.
    ("sub_body_killed", f'( x=$(sleep 30); echo no ) & p=$!; sleep 1; kill $p; wait $p; y=$(echo next); echo "$y" {R}; unset y', "next\n"),
    # A question to alias or type, and the rest of bash_parity_alias_type_lookup's kind.
    ("alias_lookup", f"alias sm='ls -l'; alias sm {R}; alias sm-missing 2>/dev/null; echo \"rc $?\" {A}; unalias sm", "alias sm='ls -l'\nrc 1\n"),
    ("alias_bad_option", f"alias -z 2>/dev/null; echo \"rc $?\" {R}", "rc 2\n"),
    ("type_missing", f"type sm-missing 2>/dev/null; echo \"rc $?\" {R}", "rc 1\n"),
    ("type_no_names", f"type; echo \"rc $?\" {R}", "rc 0\n"),
    # An alias counts only while aliases expand. The subshells keep the option from leaking.
    ("alias_option_off", f"( alias sm='ls -l'; shopt -u expand_aliases; type -t sm; echo \"rc $?\"; command -v sm; echo \"rc $?\" ) {R}", "rc 1\nrc 1\n"),
    ("alias_option_on", f"( alias sm='ls -l'; shopt -s expand_aliases; type -t sm; command -v sm ) {R}", "alias\nalias sm='ls -l'\n"),
    ("posix_turns_aliases_on", f"shopt -q expand_aliases; was=$?; ( shopt -u expand_aliases; set -o posix; shopt -q expand_aliases; echo \"rc $?\" ) {R}; shopt -q expand_aliases; [ $? = $was ] && echo same {A}; echo \"${{POSIXLY_CORRECT-unset}}\" {A}; unset was", "rc 0\nsame\nunset\n"),
    ("posix_in_subshell", f"( shopt -u expand_aliases; set -o posix; shopt -u expand_aliases; ( set -o posix; shopt -q expand_aliases; echo \"rc $?\" ) ) {R}", "rc 1\n"),
    ("type_path_not_a_file", f"type -p echo {R}; echo \"rc $?\" {A}", "rc 0\n"),
    ("escaped_pid", f"echo \"\\$\\$\" \\$\\$ {R}", "$$ $$\n"),
    ("negated_errexit", f"( set -e; ! true; ! true | cat; ! {{ true; }}; echo survived ) {R}", "survived\n"),
    # A finished frontend flushed every stream in the app, other stages' included. It
    # waited for a reader that was waiting for its end of the pipe, or for a writer on a
    # full pipe, and wrote another stage's buffered output to its own stdout.
    ("frontend_tr", f"pascal smoke/hello.pas | tr -d ' ' {R}", "pascalok42\n"),
    ("frontend_uniq", f"pascal smoke/hello.pas | uniq {R}", "pascal ok 42\n"),
    ("frontend_awk", f"pascal smoke/hello.pas | awk '{{print $2}}' {R}", "ok\n"),
    ("frontend_rev", f"pascal smoke/hello.pas | rev {R}", "24 ko lacsap\n"),
    ("frontend_wc_tr", f"pascal smoke/hello.pas | wc -l | tr -d ' ' {R}", "1\n"),
    ("clike_tr", f"clike smoke/hello.clike | tr -d ' ' {R}", "clikeok42\n"),
    ("rea_tr", f"rea smoke/hello.rea | tr -d ' ' {R}", "reaok42\n"),
    ("aether_tr", f"aether smoke/hello.aether | tr -d ' ' {R}", "aetherok42\n"),
    ("frontend_full_pipe", f"pascal smoke/big.pas | {{ sleep 1; awk 'BEGIN {{ fflush(); print \"flushed\" }}'; }} {R}", "flushed\n"),
    ("frontend_output_stays", f"pascal smoke/first.pas | {{ sleep 1; awk 'BEGIN {{ fflush(); print \"flushed\" }}'; }} {R}", "flushed\n"),
    ("frontend_full_pipe_real", f"pascal smoke/big.pas | {{ sleep 1; pascal smoke/hello.pas; }} {R}", "pascal ok 42\n"),
    ("frontend_stays_real", f"pascal smoke/first.pas | {{ sleep 1; clike smoke/hello.clike; }} {R}", "clike ok 42\n"),
    # Stages and tabs are threads of one process: one crash here takes the whole app with it.
    ("at_once_clike", f"for i in 1 2 3 4 5; do clike --no-cache smoke/v.clike | clike --no-cache smoke/x.clike; done {R}", "clike2 4 9 8\n" * 5),
    ("at_once_rea", f"for i in 1 2 3 4 5; do rea --no-cache smoke/v.rea | rea --no-cache smoke/x.rea; done {R}", "rea2 4 4 false\n" * 5),
    ("at_once_pascal", f"for i in 1 2 3 4 5; do pascal --no-cache smoke/v.pas | pascal --no-cache smoke/x.pas; done {R}", "pascal2 4 FALSE\n" * 5),
    ("at_once_aether", f"for i in 1 2 3 4 5; do aether --no-cache smoke/v.aether | aether --no-cache smoke/x.aether; done {R}", "aether2 4 x false\n" * 5),
    ("at_once_clike_pascal", f"for i in 1 2 3 4 5; do clike --no-cache smoke/v.clike | pascal --no-cache smoke/x.pas; done {R}", "pascal2 4 FALSE\n" * 5),
    ("at_once_pascal_clike", f"for i in 1 2 3 4 5; do pascal --no-cache smoke/v.pas | clike --no-cache smoke/x.clike; done {R}", "clike2 4 9 8\n" * 5),
    ("at_once_both", f"clike --no-cache smoke/v.clike | {{ clike --no-cache smoke/x.clike; cat; }} {R}", "clike2 4 9 8\nclike 3 5 7\n"),
    ("at_once_rea_aether", f"rea --no-cache smoke/v.rea | {{ aether --no-cache smoke/x.aether; cat; }} {R}", "aether2 4 x false\nrea 3 3 true\n"),
    ("at_once_aether_rea", f"aether --no-cache smoke/v.aether | {{ rea --no-cache smoke/x.rea; cat; }} {R}", "rea2 4 4 false\naether 3 b true\n"),
    # grep: the nested .exshrc context announces each background job.
    ("at_once_background", f"{{ clike --no-cache smoke/v.clike & pascal --no-cache smoke/v.pas & rea --no-cache smoke/v.rea & aether --no-cache smoke/v.aether & wait; }} | grep -v '^\\[' | sort {R}", "aether 3 b true\nclike 3 5 7\npascal 3 a TRUE\nrea 3 3 true\n"),
    # One printf, one write: clike's used to go out a byte at a time and interleave mid-line.
    ("at_once_same_lines", f"{{ clike --no-cache smoke/v.clike & clike --no-cache smoke/x.clike & clike --no-cache smoke/v.clike & clike --no-cache smoke/x.clike & wait; }} | grep -v '^\\[' | sort {R}", "clike 3 5 7\nclike 3 5 7\nclike2 4 9 8\nclike2 4 9 8\n"),
    ("at_once_bad_stage", f"pascal --no-cache smoke/bad.pas 2>/dev/null | pascal --no-cache smoke/x.pas {R}", "pascal2 4 FALSE\n"),
    ("at_once_cache", f"clike smoke/v.clike | clike smoke/v.clike {R}", "clike 3 5 7\n"),
    # A run's conventions, its interface tables and its --deny stay its own.
    ("at_once_styles", f"pascal --no-cache smoke/rt.pas | {{ rea --no-cache smoke/rt.rea; cat; }} {R}", "rea-rt 0 true\nA B 1\npascal-rt 0 TRUE\nAB1\n"),
    ("at_once_index", f"aether --no-cache smoke/rt.aether | {{ pascal --no-cache smoke/rt.pas; cat; }} {R}", "pascal-rt 0 TRUE\nAB1\naether-rt 0 true\n"),
    ("at_once_worker", f"rea --no-cache smoke/rt.rea | {{ pascal --no-cache smoke/th.pas; cat; }} {R}", "pascal-worker TRUE\npascal-main TRUE\nrea-rt 0 true\nA B 1\n"),
    ("iface_in_turn", f"pascal --no-cache smoke/if1.pas {R}; pascal --no-cache smoke/if2.pas {A}", "one:pm\ntwo:qm\ntwo-other:qn\n"),
    ("iface_at_once", f"pascal --no-cache smoke/if1.pas | {{ pascal --no-cache smoke/if2.pas; cat; }} {R}", "two:qm\ntwo-other:qn\none:pm\n"),
    ("deny_not_shared", f"pascal --no-cache --deny net smoke/rt.pas 2>/dev/null | pascal --no-cache smoke/n.pas {R}", "session ok\n"),
    ("deny_kept", f"pascal --no-cache --deny net smoke/n.pas 2>&1 | {{ pascal --no-cache smoke/rt.pas > /dev/null; cat; }} | grep -c 'denied by --deny' {R}", "1\n"),
    ("frontend_bg_reader", f"{{ sleep 10 | tr a b; }} > /dev/null 2>&1 & s=$(date +%s); pascal smoke/hello.pas {R}; e=$(date +%s); [ $((e - s)) -lt 8 ] && echo quick {A}", "pascal ok 42\nquick\n"),
    # Each writer now flushes its own output, on every way out.
    ("frontend_halt_output", f"pascal smoke/halt2.pas | cat {R}", "before halt\n"),
    ("rea_thread_output", f"rea smoke/thread.rea | tr -d ' ' | sort {R}", "childok\nmainok\n"),
    # Both ends of a local rsync run in this one process, with no fork or process exit
    # between them: a real _exit took every terminal session with it, and what one run
    # or one end sets must not reach another.
    ("rsync_version", f"rsync --version {R}; echo \"rc=$?\" {A}", "openrsync: protocol version 27\nrc=0\n"),
    ("rsync_dir", f"rsync -a smoke/rs/a smoke/rs/dir {R}; echo \"rc=$?\" {A}; cat smoke/rs/dir/a/f smoke/rs/dir/a/b/g {A}", "rc=0\nx\ny\n"),
    ("rsync_contents", f"rsync -a smoke/rs/a/ smoke/rs/con {R}; ls smoke/rs/con {A}", "b\nf\n"),
    ("rsync_dot", f"( cd smoke/rs/a; rsync -a . ../dot ) {R}; ls smoke/rs/dot {A}", "b\nf\n"),
    ("rsync_dotdot", f"( cd smoke/rs/a/b; rsync -a .. ../../up ) {R}; ls smoke/rs/up {A}", "b\nf\n"),
    ("rsync_file", f"rsync smoke/d.txt smoke/rs/one {R}; cat smoke/rs/one/d.txt {A}", "b\na\nc\n"),
    # The list holds the app's own paths, not the container's.
    ("rsync_progress", f"rsync --progress -a smoke/rs/a smoke/rs/pr 2>&1 | grep -c Documents {R}", "0\n"),
    ("rsync_error_path", f"rsync -a smoke/nope smoke/rs/ep 2>&1 | head -1 {R}", "rsync: error: /home/smoke/nope: lstat: No such file or directory\n"),
    ("rsync_abs", f"rsync -a /home/smoke/rs/a /home/smoke/rs/abs {R}; cat smoke/rs/abs/a/b/g {A}", "y\n"),
    ("rsync_in_subshell", f"( cd smoke/rs; rsync -a a sub ) {R}; cat smoke/rs/sub/a/f {A}", "x\n"),
    ("rsync_update", f"rsync -a smoke/rs/a smoke/rs/up {R}; cp smoke/d.txt smoke/rs/a/h; rsync -a smoke/rs/a smoke/rs/up {A}; cat smoke/rs/up/a/h {A}; rm smoke/rs/a/h", "b\na\nc\n"),
    # Big enough to fill the socket between the two ends, then again to send it as a delta.
    ("rsync_big", f"seq 1 200000 > smoke/rs/big; rsync smoke/rs/big smoke/rs/bd {R}; cmp smoke/rs/big smoke/rs/bd/big {A}; echo more >> smoke/rs/big; rsync smoke/rs/big smoke/rs/bd {A}; cmp smoke/rs/big smoke/rs/bd/big {A}", ""),
    ("rsync_symlink", f"ln -s f smoke/rs/a/l; rsync -a smoke/rs/a smoke/rs/sl {R}; rm smoke/rs/a/l; readlink smoke/rs/sl/a/l {A}", "f\n"),
    ("rsync_meta", f"chmod 640 smoke/rs/a/f; touch -t 202001020304 smoke/rs/a/f; rsync -a smoke/rs/a smoke/rs/me {R}; stat -c %a smoke/rs/me/a/f {A}; date -r smoke/rs/me/a/f +%Y {A}", "640\n2020\n"),
    ("rsync_delete", f"mkdir smoke/rs/del; echo z > smoke/rs/del/old; rsync -a --delete smoke/rs/a/ smoke/rs/del {R}; ls smoke/rs/del {A}", "b\nf\n"),
    ("rsync_exclude_once", f"rsync -a --exclude f smoke/rs/a/ smoke/rs/e1 > /dev/null 2>&1; rsync -a smoke/rs/a/ smoke/rs/e2 {R}; ls smoke/rs/e1 smoke/rs/e2 {A}", "smoke/rs/e1:\nb\n\nsmoke/rs/e2:\nb\nf\n"),
    ("rsync_dry_run", f"rsync -an smoke/rs/a smoke/rs/dry {R}; ls smoke/rs/dry {A}", "ls: cannot access 'smoke/rs/dry': No such file or directory\n"),
    ("rsync_pipe", f"rsync -a smoke/rs/a smoke/rs/pi | cat {R}; cat smoke/rs/pi/a/f {A}", "x\n"),
    ("rsync_background", f"rsync -a smoke/rs/a smoke/rs/bg > /dev/null 2>&1 & wait $!; echo \"wait $?\" {R}; cat smoke/rs/bg/a/f {A}", "wait 0\nx\n"),
    ("rsync_loop", f"for i in 1 2 3 4 5 6 7 8; do rsync -a smoke/rs/a smoke/rs/lp$i {A}; echo $? {A}; done", "0\n0\n0\n0\n0\n0\n0\n0\n"),
    ("rsync_two_at_once", f"rsync -a --exclude f smoke/rs/a/ smoke/rs/t1 > /dev/null 2>&1 & p=$!; rsync -a smoke/rs/a/ smoke/rs/t2 {R}; wait $p; ls smoke/rs/t1 smoke/rs/t2 {A}", "smoke/rs/t1:\nb\n\nsmoke/rs/t2:\nb\nf\n"),
    # These fail with a status, and the app carries on.
    ("rsync_no_source", f"rsync -a smoke/nope smoke/rs/no > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_bad_dest", f"rsync -a smoke/rs/a smoke/d.txt/x > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_no_server", f"rsync -a --rsync-path=nope smoke/rs/a smoke/rs/ns > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_no_shell", f"rsync -a -e nope smoke/rs/a host:x > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_usage", f"rsync > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_after_failures", f"rsync -a smoke/rs/a smoke/rs/af {R}; echo \"rc=$?\" {A}", "rc=0\n"),
    # A failed ssh under scp. It stops at the name lookup, so nothing connects.
    ("scp_no_host", f"scp smoke/d.txt nosuch.invalid:x > /dev/null 2>&1; [ $? -ne 0 ] && echo failed {R}", "failed\n"),
]
BY_NAME = {test[0]: test for test in TESTS}


def utmp_record(user: str, line: str) -> str:
    """A login in the 400-byte layout utmp_rec.c reads, in ASCII so that write_text keeps it."""
    record = bytearray(400)
    record[0] = 7  # USER_PROCESS
    record[4] = 1  # the pid
    record[8:8 + len(line)] = line.encode()
    record[40:44] = line[-4:].encode()
    record[44:44 + len(user)] = user.encode()
    record[347] = 0x65  # the time, 0x65000000 at offset 344
    return record.decode("ascii")


# Two frontends at once. Each program declares variables and routines, or a collision hides: a
# program with no names has nothing to collide. Same routine names in all of them, on purpose.
_PAD = range(1, 41)
_PAS_PAD = "".join(f"function p{n}(a: integer): integer;\nvar q: integer;\nbegin\n  q := a + 1;\n  p{n} := q;\nend;\n" for n in _PAD)
_C_PAD = "".join(f"int p{n}(int a) {{ int q; q = a + 1; return q; }}\n" for n in _PAD)
_REA_PAD = "".join(f"int p{n}(int a) {{ int q = a + 1; return q; }}\n" for n in _PAD)
_AE_PAD = "".join(f"fn p{n}(a: Int) -> Int {{ let q: Int = a + 1; ret q; }}\n" for n in _PAD)
AT_ONCE_FIXTURES = {
    "v.pas": "program V;\ntype TR = record x: integer; end;\nvar i: integer; s: string; b: boolean; r: TR;\n" + _PAS_PAD
             + "begin\n  s := 'abc'; b := true; i := p3(2); r.x := i;\n  writeln('pascal ', r.x, ' ', s[1], ' ', b);\nend.\n",
    "x.pas": "program X;\nvar j: integer; i: real; b: boolean;\n" + _PAS_PAD
             + "begin\n  j := p4(3); b := false;\n  writeln('pascal2 ', j, ' ', b);\nend.\n",
    "bad.pas": "program Bad;\nvar i: integer;\nbegin\n  i := undefinedname + 1;\nend.\n",
    "v.clike": "struct P { int x; };\nint g = 5;\nconst int K = 7;\n" + _C_PAD
               + 'int main() {\n    int i;\n    struct P* p;\n    new(&p);\n    p->x = p3(2);\n    i = p->x;\n    printf("clike %d %d %d\\n", i, g, K);\n    dispose(&p);\n    return 0;\n}\n',
    "x.clike": "struct Q { int y; int z; };\nint g = 9;\nconst int K = 8;\n" + _C_PAD
               + 'int main() {\n    int i;\n    struct Q* p;\n    new(&p);\n    p->z = p4(3);\n    i = p->z;\n    printf("clike2 %d %d %d\\n", i, g, K);\n    dispose(&p);\n    return 0;\n}\n',
    "v.rea": _REA_PAD + 'class C { int v; int get() { return my.v; } }\nint i = 3;\nC c = new C();\nc.v = p3(2);\nwriteln("rea ", c.get(), " ", i, " ", true);\n',
    "x.rea": _REA_PAD + 'class D { int w; int two() { return my.w * 2; } }\nint i = 4;\nD d = new D();\nd.w = p2(1);\nwriteln("rea2 ", d.two(), " ", i, " ", false);\n',
    "v.aether": _AE_PAD + 'fn main() -> Void {\n    let i: Int = p3(2);\n    let s = "abc";\n    let b = true;\n    fx { println("aether ", i, " ", s[1], " ", b); }\n}\n',
    "x.aether": _AE_PAD + 'fn main() -> Void {\n    let i: Int = p4(3);\n    let s = "xyz";\n    fx { println("aether2 ", i, " ", s[0], " ", false); }\n}\n',
    "rt.pas": "program Rt;\nvar k, bad: integer; s: string; b: boolean;\nbegin\n  s := 'abc'; b := true; bad := 0;\n  for k := 1 to 100000 do\n    if s[1] <> 'a' then bad := bad + 1;\n  writeln('pascal-rt ', bad, ' ', b);\n  writeln('A', 'B', 1);\nend.\n",
    "rt.rea": 'int bad = 0;\nstr s = "abc";\nint k = 0;\nwhile (k < 100000) {\n  if (s[1] != "a") { bad = bad + 1; }\n  k = k + 1;\n}\nwriteln("rea-rt ", bad, " ", true);\nwriteln("A", "B", 1);\n',
    "rt.aether": 'fn main() -> Void {\n    let s = "abc";\n    let bad: Int = 0;\n    let k: Int = 0;\n    while k < 100000 {\n        if s[0] != "a" { bad = bad + 1; }\n        k = k + 1;\n    }\n    fx { println("aether-rt ", bad, " ", true); }\n}\n',
    "th.pas": "program Th;\nvar t: integer; ok: boolean;\nprocedure Worker;\nbegin\n  ok := true;\n  writeln('pascal-worker ', ok);\nend;\nbegin\n  ok := false;\n  t := spawn Worker;\n  join t;\n  writeln('pascal-main ', ok);\nend.\n",
    "if1.pas": "program If1;\ntype\n  ILog = interface\n    procedure Say(const m: string);\n  end;\n  TLog = record\n    P: string;\n    procedure Say(const m: string);\n  end;\nprocedure TLog.Say(const m: string);\nbegin\n  writeln('one:', P, m);\nend;\nvar l: TLog; s: ILog;\nbegin\n  l.P := 'p'; s := l; s.Say('m');\nend.\n",
    "if2.pas": "program If2;\ntype\n  ILog = interface\n    procedure Other(const m: string);\n    procedure Say(const m: string);\n  end;\n  TLog = record\n    Q: string;\n    procedure Other(const m: string);\n    procedure Say(const m: string);\n  end;\nprocedure Unrelated(n: integer);\nbegin\n  writeln('unrelated ', n);\nend;\nprocedure TLog.Other(const m: string);\nbegin\n  writeln('two-other:', Q, m);\nend;\nprocedure TLog.Say(const m: string);\nbegin\n  writeln('two:', Q, m);\nend;\nvar l: TLog; s: ILog;\nbegin\n  l.Q := 'q'; s := l; s.Say('m'); s.Other('n');\nend.\n",
    "n.pas": "program N;\nvar sess: integer;\nbegin\n  sess := HttpSession();\n  if sess >= 0 then writeln('session ok');\nend.\n",
}

# Written from the host, since they are awkward to quote in a shell script.
FIXTURES = {
    "utmp.rec": utmp_record("alice", "pts/1") + utmp_record("bob", "pts/2"),
    "hello.pas": "program Hello;\nbegin\n  writeln('pascal ok ', 6*7);\nend.\n",
    "hello.clike": 'int main() {\n    printf("clike ok %d\\n", 6*7);\n    return 0;\n}\n',
    "hello.rea": 'class Main {}\n\nwriteln("rea ok ", 6*7);\n',
    "hello.aether": '@pure\nfn answer(a: Int, b: Int) -> Int {\n    ret a * b;\n}\n\nfn main() -> Void {\n    fx { println("aether ok ", answer(6, 7)); }\n    ret;\n}\n',
    "slow.pas": "program Slow;\nuses crt;\nbegin\n  delay(800);\n  writeln('slow done');\nend.\n",
    "halt.pas": "program Halting;\nbegin\n  halt(3);\nend.\n",
    "bigout.pas": "program BigOut;\nvar i: integer;\nbegin\n  for i := 1 to 30000 do writeln(i);\nend.\n",
    "bigout.clike": 'int main() {\n    int i;\n    for (i = 1; i <= 30000; i = i + 1) {\n        printf("%d\\n", i);\n    }\n    return 0;\n}\n',
    "big.pas": "program Big;\nvar i: integer;\nbegin\n  for i := 1 to 100000 do writeln('line of big output');\nend.\n",
    "first.pas": "program First;\nuses crt;\nbegin\n  writeln('first stage');\n  delay(2000);\nend.\n",
    "halt2.pas": "program Halting2;\nbegin\n  writeln('before halt');\n  halt(3);\nend.\n",
    "thread.rea": 'void child() {\n    writeln("child ok");\n}\n\nint t = spawn child();\njoin t;\nwriteln("main ok");\n',
    **AT_ONCE_FIXTURES,
}

# The first lines of auto.sh, run in the app before any test.
SETUP = [
    'cd "$HOME"',
    "mkdir smoke/res smoke/sub smoke/sub2",
    "printf 'b\\na\\nc\\n' > smoke/d.txt",
    "printf 'a\\na\\nb\\n' > smoke/u.txt",
    "printf 'one:two\\n' > smoke/c.txt",
    "printf 'echo script\\n' > smoke/s.sh",
    "cp smoke/c.txt smoke/sub2/x",
    "printf 'needle\\n' > smoke/sub/n.txt",
    "cp Examples/tiny/max.tiny smoke/max.tiny",
    "cp Examples/tiny/countdown.tiny smoke/cd.tiny",
    "printf 'pascal smoke/slow.pas >> smoke/res/script_order.txt 2>&1\\necho after-pascal >> smoke/res/script_order.txt\\nclike smoke/hello.clike >> smoke/res/script_order.txt 2>&1\\necho after-clike >> smoke/res/script_order.txt\\n' > smoke/ord.sh",
    "printf '#!/bin/exsh\\npascal smoke/slow.pas >> smoke/res/shebang_order.txt 2>&1\\necho after-pascal >> smoke/res/shebang_order.txt\\n' > smoke/ord_sb.sh",
    "printf 'pascal smoke/slow.pas >> smoke/res/source_order.txt 2>&1\\necho after-pascal >> smoke/res/source_order.txt\\n' > smoke/ord_src.sh",
    "printf '#!/bin/exsh\\necho sb $#\\n' > smoke/sb.sh",
    "printf '#!/bin/exsh\\nexit 3\\n' > smoke/sb3.sh",
    "printf '#!/bin/exsh\\nset -- x y\\necho $1 $2\\n' > smoke/sbset.sh",
    "printf '#!/bin/exsh\\necho $1 $2\\n' > smoke/pos.sh",
    "printf '#!/bin/exsh\\necho ${1}/${2}\\n' > smoke/posb.sh",
    "printf '#!/bin/exsh\\necho $1.txt\\n' > smoke/posw.sh",
    "printf 'x := 6 * 7;\\nwrite x\\n' > smoke/p.tiny",
    "chmod +x smoke/ord_sb.sh smoke/sb.sh smoke/sb3.sh smoke/sbset.sh smoke/pos.sh smoke/posb.sh smoke/posw.sh",
    "mkdir -p smoke/rs/a/b",
    "printf 'x\\n' > smoke/rs/a/f",
    "printf 'y\\n' > smoke/rs/a/b/g",
]


class Known(NamedTuple):
    """A failure the release notes list: its issue and the output it gives (None: no result file)."""

    issue: str
    got: Optional[str]
    always: bool = True  # False: it comes and goes, so a pass is no news.


# The Known Issues in RELEASE_NOTES_V.1R32.md that these tests hit. A test here
# that fails any other way is an unexpected failure. When one is fixed, drop it
# here and from the next release's notes.
KNOWN = {
    "tiny_full": Known(
        "the VM's file builtins do not translate the app's absolute paths",
        "MStreamSaveToFile: Cannot open file '/home/smoke/cd3.pbc' for writing.\n"
        "ls: cannot access 'smoke/cd3.pbc': No such file or directory\n",
    ),
}


class SetupError(Exception):
    """The run could not be set up or the simulator refused a command."""


class Device(NamedTuple):
    udid: str
    name: str
    ios: Tuple[int, int]
    state: str

    def __str__(self) -> str:
        return f"{self.name}, iOS {self.ios[0]}.{self.ios[1]} ({self.udid})"


class Run(NamedTuple):
    """One launch of the app."""

    state: str  # done, died or stalled
    seconds: float
    last_step: Optional[str]
    crashes: List[Path]


def simctl(*args: str, timeout: float = 300) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(["xcrun", "simctl", *args], text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SetupError(f"simctl {args[0]} did not finish in {timeout:.0f} s") from None


def find_app(given: Optional[str]) -> Tuple[Path, dict]:
    if given:
        app = Path(given).expanduser().resolve()
    else:
        found = [ROOT / "build/DerivedData-ios-sim" / RELEASE_BUILD]
        found += Path.home().glob(f"Library/Developer/Xcode/DerivedData/*/{RELEASE_BUILD}")
        found = [path for path in found if (path / "PSCAL").exists()]
        if not found:
            raise SetupError("no Release simulator build found: build one (see ios/README.md) or pass --app")
        app = max(found, key=lambda path: (path / "PSCAL").stat().st_mtime)
    try:
        with open(app / "Info.plist", "rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException) as error:
        raise SetupError(f"not an app bundle: {app}: {error}") from None
    if "iPhoneSimulator" not in info.get("CFBundleSupportedPlatforms", []):
        raise SetupError(f"not a simulator build: {app}")
    return app, info


def list_devices() -> List[Device]:
    listing = simctl("list", "devices", "available", "-j")
    if listing.returncode:
        raise SetupError(f"simctl list failed: {listing.stderr.strip()}")
    devices = []
    for runtime, entries in json.loads(listing.stdout)["devices"].items():
        version = re.search(r"\.iOS-(\d+)-(\d+)", runtime)
        if version:
            ios = (int(version[1]), int(version[2]))
            devices += [Device(entry["udid"], entry["name"], ios, entry["state"]) for entry in entries]
    return devices


def has_app(device: Device, app: Path) -> bool:
    return any(DEVICES.glob(f"{device.udid}/data/Containers/Bundle/Application/*/{app.name}"))


def find_device(given: Optional[str], app: Path) -> Device:
    devices = list_devices()
    if given:
        matches = [device for device in devices if given in (device.udid, device.name)]
        if not matches:
            raise SetupError(f"no available iOS simulator is called {given}")
        return max(matches, key=lambda device: device.ios)
    # Removing the app deletes its workspace, so leave devices that have it alone.
    # A shut-down device is one nothing else is using.
    candidates = [device for device in devices
                  if device.name.startswith(("iPad", "iPhone")) and not has_app(device, app)]
    if not candidates:
        raise SetupError("every iPad and iPhone simulator already has the app: choose one with --device")
    return min(candidates, key=lambda device: (device.state == "Booted", (-device.ios[0], -device.ios[1]),
                                               not device.name.startswith("iPad")))


def report_is_for(report: Path, udid: str) -> bool:
    try:
        body = json.loads(report.read_text(errors="replace").split("\n", 1)[1])
    except (OSError, IndexError, ValueError):
        return True
    return udid in body.get("procPath", udid)


def describe_crash(report: Path) -> str:
    try:
        body = json.loads(report.read_text(errors="replace").split("\n", 1)[1])
        exception = body.get("exception", {})
        frames = body["threads"][body["faultingThread"]]["frames"]
        images = body.get("usedImages", [])
    except (OSError, IndexError, KeyError, TypeError, ValueError):
        return str(report)
    names = [frame.get("symbol") or images[frame["imageIndex"]].get("name", "?") for frame in frames[:8]]
    kind = " ".join(filter(None, (exception.get("type"), exception.get("signal"), exception.get("subtype"))))
    return f"{report}\n   {kind}\n   " + " < ".join(names)


class Session:
    """The app installed on one simulator, for the length of a run."""

    def __init__(self, app: Path, info: dict, device: Device) -> None:
        self.app = app
        self.bundle = info["CFBundleIdentifier"]
        self.device = device
        self.booted = False
        self.installed = False
        self.home = Path()
        self.smoke = Path()

    def start(self, replace_installed: bool) -> None:
        udid = self.device.udid
        if self.device.state == "Shutdown":
            self.booted = True
            booted = simctl("boot", udid, timeout=600)
            if booted.returncode:
                raise SetupError(f"could not boot {self.device}: {booted.stderr.strip()}")
        ready = simctl("bootstatus", udid, timeout=600)
        if ready.returncode:
            raise SetupError(f"{self.device} did not finish booting: {ready.stderr.strip()}")

        if has_app(self.device, self.app):
            data = simctl("get_app_container", udid, self.bundle, "data").stdout.strip()
            rc = Path(data) / "Documents/home/.exshrc"
            ours = rc.exists() and HOOK in rc.read_text(errors="replace")
            if not ours and not replace_installed:
                raise SetupError(f"{self.bundle} is already on {self.device} and removing it deletes its "
                                 "workspace: choose another device, or pass --replace-installed")
        # A fresh container, like a first install from TestFlight. This also clears
        # the record a rebooted simulator can keep of an app whose files are gone.
        simctl("terminate", udid, self.bundle)
        simctl("uninstall", udid, self.bundle)
        if has_app(self.device, self.app):
            raise SetupError(f"could not remove the old app from {self.device}")
        installed = simctl("install", udid, str(self.app))
        if installed.returncode:
            raise SetupError(f"install failed: {installed.stderr.strip()}")
        self.installed = True
        container = simctl("get_app_container", udid, self.bundle, "data")
        if container.returncode:
            raise SetupError(f"no app container: {container.stderr.strip()}")
        self.home = Path(container.stdout.strip()) / "Documents/home"
        self.smoke = self.home / "smoke"
        self.home.mkdir(parents=True, exist_ok=True)
        # The app keeps an .exshrc that already exists, so write the one it would
        # seed on first launch, plus the hook.
        skel = self.app / "etc/skel/.exshrc"
        (self.home / ".exshrc").write_text((skel.read_text() if skel.exists() else "") + f"\n{HOOK}\n")

    def finish(self, keep: bool) -> None:
        if keep:
            print(f"kept: the app on {self.device}, home {self.home}")
            return
        try:
            if self.installed:
                simctl("terminate", self.device.udid, self.bundle)
                removed = simctl("uninstall", self.device.udid, self.bundle)
                if removed.returncode:
                    print(f"warning: could not uninstall the app: {removed.stderr.strip()}", file=sys.stderr)
            if self.booted:
                # Shut down right after an uninstall, a simulator lists the app again on its next boot.
                time.sleep(5)
                stopped = simctl("shutdown", self.device.udid)
                if stopped.returncode:
                    print(f"warning: could not shut down {self.device}: {stopped.stderr.strip()}", file=sys.stderr)
        except SetupError as error:
            print(f"warning: {error}", file=sys.stderr)

    def run(self, lines: Sequence[str], timeout: float) -> Run:
        """Launch the app once with these lines after SETUP, on a clean smoke directory."""
        udid = self.device.udid
        simctl("terminate", udid, self.bundle)
        shutil.rmtree(self.smoke, ignore_errors=True)
        self.smoke.mkdir(parents=True)
        for name, body in FIXTURES.items():
            (self.smoke / name).write_text(body)
        (self.smoke / "auto.sh").write_text("\n".join([*SETUP, *lines, "touch smoke/AUTODONE"]) + "\n")

        before = set(REPORTS.glob("PSCAL-*.ips"))
        launched = simctl("launch", udid, self.bundle)
        if launched.returncode:
            raise SetupError(f"launch failed: {launched.stderr.strip()}")
        launched_pid = re.search(r":\s*(\d+)\s*$", launched.stdout)
        if not launched_pid:
            raise SetupError(f"no process id in simctl's answer: {launched.stdout.strip()}")
        pid = int(launched_pid[1])
        started = time.time()
        state = self._wait(pid, timeout)
        seconds = time.time() - started
        simctl("terminate", udid, self.bundle)
        # Reports land a little after the process goes.
        return Run(state, seconds, self._last_step(), self._new_crashes(before, 20 if state == "died" else 2))

    def _wait(self, pid: int, timeout: float) -> str:
        done = self.smoke / "AUTODONE"
        progress = self.smoke / "progress.txt"
        seen = -1
        last_change = time.time()
        while not done.exists():
            size = progress.stat().st_size if progress.exists() else 0
            if size != seen:
                seen, last_change = size, time.time()
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return "died"
            except PermissionError:
                pass
            if time.time() - last_change > timeout:
                return "stalled"
            time.sleep(0.3)
        return "done"

    def _last_step(self) -> Optional[str]:
        try:
            return (self.smoke / "progress.txt").read_text().split()[-1].lstrip("@")
        except (OSError, IndexError):
            return None

    def _new_crashes(self, before: set, wait: float) -> List[Path]:
        deadline = time.time() + wait
        while True:
            new = [report for report in set(REPORTS.glob("PSCAL-*.ips")) - before
                   if report_is_for(report, self.device.udid)]
            if new or time.time() >= deadline:
                return sorted(new)
            time.sleep(0.5)

    def result(self, name: str) -> Optional[str]:
        try:
            return NOISE.sub("", (self.smoke / "res" / f"{name}.txt").read_text(errors="replace"))
        except OSError:
            return None


def script_lines(tests: Sequence[Tuple[str, str, Optional[str]]]) -> List[str]:
    lines = []
    for name, command, _ in tests:
        lines.append(f'echo "@{name}" >> smoke/progress.txt')
        lines.append(command.replace("{o}", f"smoke/res/{name}.txt"))
    return lines


def explain_stop(run: Run, timeout: float) -> str:
    where = f"in {run.last_step}" if run.last_step else "before any test started: was ~/.exshrc sourced?"
    if run.state == "died":
        return f"UNEXPECTED: the app died {where}"
    return f"UNEXPECTED: no progress for {timeout:.0f} s {where}"


def run_tests(session: Session, tests: Sequence, timeout: float, verbose: bool) -> int:
    run = session.run(script_lines(tests), timeout)
    print(f"finished in {run.seconds:.0f} s" if run.state == "done" else explain_stop(run, timeout))
    names = [test[0] for test in tests]
    reached = len(names) if run.state == "done" or run.last_step not in names else names.index(run.last_step) + 1
    passed = known_failures = unexpected = 0
    for name, command, want in tests[:reached]:
        got = session.result(name)
        known = KNOWN.get(name)
        shown = command.replace("{o}", f"smoke/res/{name}.txt")
        if want is None and got is not None:
            passed += 1
            if verbose:
                print(f"info {name}: {got[:140]!r}")
        elif got == want:
            passed += 1
            if known and known.always:
                print(f"UNEXPECTED PASS {name}: remove it from KNOWN if this is fixed: {known.issue}")
        elif known and got == known.got:
            known_failures += 1
            if verbose:
                print(f"known {name}: {known.issue}")
        else:
            unexpected += 1
            print(f"FAIL {name}: {shown}\n   want {want!r}\n   got  " + ("no result file" if got is None else repr(got[:220])))
    skipped = len(tests) - reached
    if skipped:
        print(f"did not run: {skipped} tests after {names[reached - 1]}")
    for report in run.crashes:
        print("NEW CRASH REPORT " + describe_crash(report))
    print(f"{len(tests)} tests: {passed} passed, {known_failures} known issues, {unexpected} unexpected, "
          f"{skipped} not run, {len(run.crashes)} new crash reports")
    return 1 if unexpected or skipped or run.crashes or run.state != "done" else 0


def run_probes(session: Session, probes: Sequence[str], timeout: float) -> int:
    """One launch per probe, so that a crash is attributed to the command that caused it."""
    status = 0
    for number, probe in enumerate(probes, 1):
        name, command = (probe, BY_NAME[probe][1]) if probe in BY_NAME else (f"probe{number}", probe)
        run = session.run(script_lines([(name, command, None)]), timeout)
        print(f"{name:14} {run.state:8} crashes={len(run.crashes)} out={(session.result(name) or '')[-300:]!r}")
        for report in run.crashes:
            print("NEW CRASH REPORT " + describe_crash(report))
        if run.crashes or run.state != "done":
            status = 1
    return status


def run_bisect(session: Session, target: str, timeout: float) -> int:
    """Find which earlier test makes the target fail, by binary search on a prefix."""
    stop = next(index for index, test in enumerate(TESTS) if test[0] == target)
    want = TESTS[stop][2]
    if want is None:
        raise SetupError(f"{target} has no expected output to bisect on")

    def passes(chosen: Sequence[int]) -> bool:
        session.run(script_lines([TESTS[index] for index in chosen] + [TESTS[stop]]), timeout)
        return session.result(target) == want

    if passes(range(stop)):
        print(f"{target} passes after all {stop} earlier tests: nothing to bisect")
        return 0
    if not passes([]):
        print(f"{target} fails on its own")
        return 0
    low, high = 0, stop  # With the first `low` tests it passes, with the first `high` it fails.
    while high - low > 1:
        middle = (low + high) // 2
        ok = passes(range(middle))
        print(f"first {middle} tests, ending with {TESTS[middle - 1][0]}: {'ok' if ok else 'FAIL'}")
        if ok:
            low = middle
        else:
            high = middle
    culprit = TESTS[high - 1]
    print(f"{target} starts failing after {culprit[0]}: {culprit[1]}")
    print(f"with only {culprit[0]} before it: {'ok' if passes([high - 1]) else 'FAIL'}")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run commands inside the iOS app in a simulator and compare their output.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Run it from a checkout with a Release simulator build; ios/README.md has the build commands.

--probe is for crashes: --probe 'for i in 1 2 3; do split -l 1 smoke/d.txt smoke/q.; cat smoke/q.aa >> {o} 2>&1; done'
--bisect is for a test that fails only in a long run: --bisect awk_lt""",
    )
    parser.add_argument("--app", help="the PSCAL.app to test (default: the newest Release simulator build)")
    parser.add_argument("--device", help="simulator UDID or name (default: a shut-down iPad or iPhone without the app, "
                                         "on the newest iOS)")
    parser.add_argument("--timeout", type=float, default=180, metavar="SECONDS",
                        help="give up after this long with no test starting (default: %(default)s)")
    parser.add_argument("--keep", action="store_true", help="leave the app installed and the simulator booted, to look around")
    parser.add_argument("--replace-installed", action="store_true",
                        help="allow removing a copy of the app that this tool did not install, and its workspace")
    parser.add_argument("-v", "--verbose", action="store_true", help="also print known failures and tests with no expected output")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--list", action="store_true", help="print the test names and exit")
    mode.add_argument("--only", metavar="NAMES", help="run these tests, comma separated; some need files an earlier test makes")
    mode.add_argument("--probe", action="append", metavar="COMMAND",
                      help="run a test by name, or a command, in a launch of its own and report whether the app "
                           "survived; {o} is its result file; can be repeated")
    mode.add_argument("--bisect", metavar="NAME", help="find which earlier test makes this test fail")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.list:
        print("\n".join(BY_NAME))
        return 0
    named = args.only.split(",") if args.only else [args.bisect] if args.bisect else []
    unknown = [name for name in named if name not in BY_NAME]
    if unknown:
        print(f"unknown test: {', '.join(unknown)} (see --list)", file=sys.stderr)
        return 2

    # So that a killed run still uninstalls the app and shuts the simulator down.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    session = None
    try:
        app, info = find_app(args.app)
        device = find_device(args.device, app)
        built = time.strftime("%Y-%m-%d %H:%M", time.localtime((app / "PSCAL").stat().st_mtime))
        print(f"app: {app} (build {info.get('CFBundleVersion')}, linked {built})\ndevice: {device}")
        session = Session(app, info, device)
        session.start(args.replace_installed)
        if args.probe:
            return run_probes(session, args.probe, args.timeout)
        if args.bisect:
            return run_bisect(session, args.bisect, args.timeout)
        tests = [BY_NAME[name] for name in named] if named else TESTS
        return run_tests(session, tests, args.timeout, args.verbose)
    except SetupError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    finally:
        if session:
            session.finish(args.keep)


if __name__ == "__main__":
    sys.exit(main())
