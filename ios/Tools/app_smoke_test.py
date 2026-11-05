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
import socket
import subprocess
import sys
import tempfile
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
O = '"$HOME/{o}"'  # The result file, from another directory.

# What vmfiles and vmshow, in SETUP, print for a directory whose copies nothing touched.
V2 = "smoke/v2/vm.del: del2\nsmoke/v2/vm.in: in2\nsmoke/v2/vm.ren: ren2\n"
VH = "vm.del: delH\nvm.in: inH\nvm.ren: renH\n"

# A server that never says a word: the system takes the connection, and nothing here reads it.
SILENT_SERVER = socket.create_server(("127.0.0.1", 0))
SILENT_PORT = SILENT_SERVER.getsockname()[1]

PRIMES = "Find all prime numbers up to: Prime numbers found up to 10:\n2 3 5 7 \n\nDone.\n"

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
    # [ and test compare strings; patterns and nocasematch are for [[ and case.
    ("test_literal", f"( [ abc = 'a*' ]; echo $?; [ '[5]' = '[5]' ]; echo $?; [ '[5]' != '[5]' ]; echo $?; test '[5]' = '[5]'; echo $?; v='*'; [ abc = \"$v\" ]; echo $?; [ \"$v\" = \"$v\" ]; echo $? ) {R}", "1\n0\n1\n0\n1\n0\n"),
    ("test_literal_cond", f"( [[ abc = a* ]]; echo $?; [[ abc = 'a*' ]]; echo $?; [[ '[5]' = '[5]' ]]; echo $?; shopt -s nocasematch; [ ABC = abc ]; echo $?; [[ ABC = abc ]]; echo $?; case ABC in abc) echo c;; esac; shopt -u nocasematch ) {R}", "0\n1\n0\n1\n0\nc\n"),
    ("test_operator_value", f"( v='!'; [ \"$v\" = x ]; echo $?; [ \"$v\" != \"$v\" ]; echo $?; [ a = ] 2>/dev/null; echo $?; [ a b ] 2>/dev/null; echo $?; builtin [ a == a ]; echo $? ) {R}", "1\n1\n2\n2\n0\n"),
    ("test_applet", f"( env test a == a; echo $?; env test '!' = x; echo $?; env test abc = 'a*'; echo $?; env test '[5]' = '[5]'; echo $? ) {R}", "0\n1\n1\n0\n"),
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
    ("parity_stdin_redirect_loop",
     f"{{ i=0; while [ \"$i\" -lt 300 ]; do cat < smoke/d.txt > /dev/null || break; i=$((i + 1)); done; echo \"$i\"; }} < smoke/d.txt {R}", "300\n"),
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
    # Threads that start a loop together: a mix-up shows as a `shell loop:` message, a short count or a hang.
    ("loops_together_pipe", f": > smoke/ltp.out; n=0; while [ $n -lt 300 ]; do for f in 1 2; do echo $f; done | while read x; do echo $x >> smoke/ltp.out; done; n=$((n+1)); done {R}; wc -l < smoke/ltp.out | tr -d ' ' {A}", "600\n"),
    ("loops_together_job", f": > smoke/ltj.out; n=0; while [ $n -lt 300 ]; do for f in 1; do echo $f >> smoke/ltj.out; done & i=0; until [ $i -ge 1 ]; do i=$((i+1)); done; n=$((n+1)); done > smoke/ltj.msg 2>&1; wait; grep -v '^\\[' smoke/ltj.msg {R}; wc -l < smoke/ltj.out | tr -d ' ' {A}", "300\n"),
    # libc answers dirname in one buffer for the whole process, so one could print another's answer.
    ("dirname_together", f": > smoke/dn1; : > smoke/dn2; : > smoke/dn3; : > smoke/dn4; n=0; while [ $n -lt 300 ]; do dirname /aaa/one >> smoke/dn1 & dirname /bbb/two >> smoke/dn2 & dirname /ccc/three >> smoke/dn3 & dirname /ddd/four >> smoke/dn4 & wait; n=$((n+1)); done > smoke/dn.msg 2>&1; echo \"$(grep -c -x /aaa smoke/dn1) $(grep -c -x /bbb smoke/dn2) $(grep -c -x /ccc smoke/dn3) $(grep -c -x /ddd smoke/dn4)\" {R}", "300 300 300 300\n"),
    # libc's getopt keeps its place for the whole process: tee lost its -a, made a file named -a or read past its arguments.
    ("tee_together", f"rm -rf smoke/tt; mkdir -p smoke/tt; a=\"-a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a -a\"; n=0; while [ $n -lt 300 ]; do echo x | tee $a smoke/tt/t1 > /dev/null & echo x | tee $a smoke/tt/t2 > /dev/null & echo x | tee $a smoke/tt/t3 > /dev/null & echo x | tee $a smoke/tt/t4 > /dev/null & wait; n=$((n+1)); done > smoke/tt.msg 2>&1; echo \"$(wc -l < smoke/tt/t1 | tr -d ' ') $(wc -l < smoke/tt/t2 | tr -d ' ') $(wc -l < smoke/tt/t3 | tr -d ' ') $(wc -l < smoke/tt/t4 | tr -d ' ')\" {R}; ls -A | grep -c -x -- -a {A}", "300 300 300 300\n0\n"),
    ("applet_bad_option", f"tee -z < /dev/null 2>&1 | head -1 {R}", "tee: invalid option -- 'z'\n"),
    ("applet_missing_arg", f"mknod -m 2>&1 | head -1 {R}", "mknod: option requires an argument -- 'm'\n"),
    ("rsync_bad_option", f"rsync --nosuch 2>&1 | head -1 {R}", "rsync: bad option '--nosuch'\n"),
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
    # A simple job keeps what it changes, as a compound one does. The directory goes back within the
    # row, so a failure here does not move the rows after it.
    # A DEBUG trap ran between a pipeline's stages, which ended the app.
    ("debug_trap_pipeline", f"jn=0; trap 'jn=$((jn+1))' DEBUG; echo a | tr a b | cat {R}; trap - DEBUG; echo \"$jn\" {A}; unset jn", "b\n4\n"),
    ("debug_trap_group_stage", f"jn=0; trap 'jn=$((jn+1))' DEBUG; {{ true; }} | cat; trap - DEBUG; echo \"$jn\" {R}; unset jn", "2\n"),
    ("debug_trap_word_timing", f"unset jv; trap 'jv=$((jv+1))' DEBUG; echo \"first $jv\" | cat {R}; trap - DEBUG; unset jv", "first 1\n"),
    ("debug_trap_again_and_again", f"trap 'jv=1' DEBUG; for ji in 1 2 3 4 5 6 7 8 9 10 11 12; do echo a | cat | cat > /dev/null; done; trap - DEBUG; unset jv; echo alive {R}", "alive\n"),
    # A stage's word that failed ended the shell under a running stage, which ended the app.
    ("stage_word_fails_function", f"( set -u; jf() {{ sleep 0.01 | cat $jwn; echo \"in $?\" {R}; }}; jf ) 2>/dev/null", "in 1\n"),
    ("stage_word_fails_eval", f"( set -u; eval 'sleep 0.01 | cat $jwn; echo \"in $?\" {R}' ) 2>/dev/null", "in 1\n"),
    ("stage_word_fails_nested_sh", f"sh -c 'set -u; sleep 0.01 | cat $jwn' 2>/dev/null; echo alive {R}", "alive\n"),
    ("stage_word_fails_again_and_again", f"for ji in 1 2 3 4 5 6 7 8 9 10 11 12; do ( set -u; jf() {{ sleep 0.005 | cat $jwn; }}; jf ) 2>/dev/null; done; echo alive {R}", "alive\n"),
    ("job_own_var", f"J=1; jv() {{ J=2; }}; jv & wait; echo \"[$J]\" {R}", "[1]\n"),
    ("job_own_read", f"J=1; read J <<< x & wait; echo \"[$J]\" {R}", "[1]\n"),
    ("job_own_cd", f"jd=$PWD; cd smoke & wait; jp=$PWD; cd \"$jd\"; [ \"$jp\" = \"$jd\" ] && echo stayed {R}", "stayed\n"),
    ("job_own_cd_func", f"jd=$PWD; jg() {{ cd /; }}; jg & wait; jp=$PWD; cd \"$jd\"; [ \"$jp\" = \"$jd\" ] && echo stayed {R}", "stayed\n"),
    ("job_own_cd_moves_it", f"jg() {{ cd smoke; echo x > jm.txt; }}; jg & wait; cat smoke/jm.txt {R}", "x\n"),
    ("job_own_has_old_def", f"jf() {{ echo old {A}; }}; : {R}; jf & jf() {{ echo new {A}; }}; wait", "old\n"),
    ("job_own_mask", f"umask 022; js() {{ sleep 0.2; }}; js & umask 077; wait; umask {R}; umask 022", "0077\n"),
    ("job_own_mask_modes", f"umask 022; jm() {{ umask 077; : > smoke/jmj; }}; jm & wait; : > smoke/jms; ls -l smoke/jmj smoke/jms | cut -c1-10 {R}", "-rw-------\n-rw-r--r--\n"),
    ("job_own_called_from", f"jg() {{ echo \"${{FUNCNAME[1]}}\" {R}; }}; jf() {{ jg & wait; }}; jf", "jf\n"),
    # So does a pipeline stage, wherever it stands.
    ("stage_own_cd", f"jd=$PWD; cd smoke | cat; jp=$PWD; cd \"$jd\"; [ \"$jp\" = \"$jd\" ] && echo stayed {R}", "stayed\n"),
    ("stage_own_cd_last", f"jd=$PWD; true | cd smoke; jp=$PWD; cd \"$jd\"; [ \"$jp\" = \"$jd\" ] && echo stayed {R}", "stayed\n"),
    ("stage_own_read", f"J=1; echo x | read J; echo \"[$J]\" {R}", "[1]\n"),
    ("stage_own_mask", f"umask 022; umask 077 | cat; umask {R}", "0022\n"),
    ("stage_own_exit", f"true | exit 3; echo \"next ${{PIPESTATUS[1]}}\" {R}", "next 3\n"),
    # A job with its output redirected lets go of the pipe it replaced: the reader sees end of
    # file while the job runs. The job marks a held pipe and gives up, so this cannot hang.
    ("background_release", f"( {{ : > smoke/rel.up; i=0; while [ ! -e smoke/rel.seen ] && [ $i -lt 30 ]; do i=$((i+1)); sleep 0.1; done; [ -e smoke/rel.seen ] || : > smoke/rel.held; }} > /dev/null 2>&1 & until [ -e smoke/rel.up ]; do sleep 0.05; done ) | {{ cat > /dev/null; if [ -e smoke/rel.held ]; then echo held; else echo released; fi; : > smoke/rel.seen; }} {R}; wait", "released\n"),
    # A job outlives what started it, with the stdout and descriptors that had at the `&`. Its
    # thread used to read the starter's destroyed vproc, and a crash here ends the whole app.
    # grep: the nested .exshrc context announces each background job, here into the pipe.
    ("background_starter_gone", f"for i in 1 2 3 4 5 6 7 8 9 10; do ( {{ echo job; }} & ) | grep -v '^\\[' | tr a-z A-Z {A}; done", "JOB\n" * 10),
    ("background_eval_gone", f"for i in 1 2 3 4 5 6 7 8 9 10; do ( eval \"{{ echo job; }} &\" ) | grep -v '^\\[' | tr a-z A-Z {A}; done", "JOB\n" * 10),
    # A substitution moves only its own shell's stdout. It used to move host fd 1, the whole app's,
    # so a job's substitution could cost the shell its output. And a job killed inside one must let
    # go of the pipe it was writing to, or `cat` never returns.
    ("substitution_beside_job", f"( i=0; while [ $i -lt 50 ]; do x=$(echo hi); i=$((i+1)); done ) & for k in 1 2 3 4 5; do y=$(echo next); echo \"$y\" {A}; done; wait", "next\n" * 5),
    ("substitution_two_jobs", f"for j in 1 2; do ( i=0; bad=0; while [ $i -lt 50 ]; do x=$(echo hi); [ \"$x\" = hi ] || bad=$((bad+1)); i=$((i+1)); done; echo \"$bad wrong\" {A} ) & done; wait", "0 wrong\n0 wrong\n"),
    ("substitution_killed_job_pipe", f"echo '( x=$(sleep 2); echo no ) & p=$!; sleep 0.05; kill $p; wait $p; echo \"killed $?\"' > smoke/ss3.sh; sh smoke/ss3.sh | cat {R}", "killed 143\n"),
    ("substitution_killed_job_sourced", f"echo '( x=$(sleep 2); echo no ) & p=$!; sleep 0.05; kill $p; wait $p; echo \"killed $?\"' > smoke/ss4.sh; . smoke/ss4.sh | cat {R}", "killed 143\n"),
    ("background_eval_wait", f"eval \"{{ exit 3; }} &\"; wait $!; echo \"wait $?\" {R}", "wait 3\n"),
    ("background_subshell_wait", f"( eval \"{{ exit 5; }} &\"; wait $!; echo \"wait $?\" ) | grep -v '^\\[' {R}", "wait 5\n"),
    ("background_starter_fd", f"exec 3> smoke/bs3.txt; ( {{ echo x >&3; }} & ) | cat; exec 3>&-; cat smoke/bs3.txt {R}", "x\n"),
    ("background_starter_copy", f"exec 3> smoke/bs4.txt; ( {{ sleep 0.1; echo kept >&3; }} & exec 3>&- ) | cat; exec 3>&-; cat smoke/bs4.txt {R}", "kept\n"),
    ("subshell_exit_trap_redirect", f"( trap 'echo bye' EXIT; echo body ) > smoke/et.txt; cat smoke/et.txt {R}", "body\nbye\n"),
    ("stage_redirect_undone", f"echo a | {{ cat; echo b; }} > smoke/st.txt; echo back {R}; cat smoke/st.txt {A}", "back\na\nb\n"),
    # Where a simple job's output went. Each file is read back with a prefix, and a job in a redirected group prints `[N] pid` on its stderr.
    ("bg_redir_above_stdio", f"echo x 3> smoke/bgr1.txt {R} & wait; if [ -e smoke/bgr1.txt ]; then echo made; else echo none; fi {A}", "x\nmade\n"),
    ("bg_redir_to_stderr", f"{{ echo err >&2 & wait; }} > smoke/bgr2.o 2> smoke/bgr2.e; sed 's/^/o: /' smoke/bgr2.o {R}; grep -v '^\\[' smoke/bgr2.e | sed 's/^/e: /' {A}", "e: err\n"),
    ("bg_redir_to_fd", f"exec 3> smoke/bgr3.txt; echo x >&3 & wait; exec 3>&-; sed 's/^/f: /' smoke/bgr3.txt {R}", "f: x\n"),
    ("bg_redir_err_to_out", f"{{ sh -c 'echo err >&2' 2>&1 & wait; }} > smoke/bgr4.o 2> smoke/bgr4.e; sed 's/^/o: /' smoke/bgr4.o {R}; grep -v '^\\[' smoke/bgr4.e | sed 's/^/e: /' {A}", "o: err\n"),
    ("bg_redir_order", f"{{ sh -c 'echo out; echo err >&2' 2>&1 > smoke/bgr5.f & wait; }} > smoke/bgr5.o 2> smoke/bgr5.e; sed 's/^/f: /' smoke/bgr5.f {R}; sed 's/^/o: /' smoke/bgr5.o {A}; grep -v '^\\[' smoke/bgr5.e | sed 's/^/e: /' {A}", "f: out\no: err\n"),
    ("bg_redir_dup_in", f"exec 3< smoke/d.txt; cat <&3 {R} & wait; exec 3<&-", "b\na\nc\n"),
    ("bg_redir_here_string", f"cat <<< hi {R} & wait", "hi\n"),
    ("bg_redir_here_doc", f"cat <<BGR {R} &\nhere\nBGR\nwait", "here\n"),
    ("bg_redir_in_file", f"cat < smoke/d.txt > smoke/bgr7.txt & wait; sed 's/^/f: /' smoke/bgr7.txt {R}", "f: b\nf: a\nf: c\n"),
    ("bg_redir_no_stdin", f"sh -c 'cat & wait $!; echo \"rc $?\"' {R}", "rc 0\n"),
    ("bg_redir_close", f"echo x >&- 2> /dev/null & wait $!; echo \"rc $?\" {R}", "rc 1\n"),
    ("bg_redir_close_err", f"echo x 2>&- >&2 & wait $!; echo \"rc $?\" {R}", "rc 1\n"),
    ("redir_close_stdout", f"echo x >&- 2> /dev/null; echo \"rc $?\" {R}", "rc 1\n"),
    ("bg_redir_bad_fd", f"echo x 2> /dev/null >&7 & wait $!; echo \"rc $?\" {R}", "rc 1\n"),
    ("bg_redir_stays_with_job", f"echo x 3> smoke/bgr6.txt {R} & wait; {{ echo y >&3; }} 2> /dev/null; echo \"rc $?\" {A}", "x\nrc 1\n"),
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
    # env reached a raw exec for anything but an applet, and that replaced the whole app. These
    # are the positions a script can be in; the env_sh rows further down cover the rest.
    ("env_script", f"env smoke/sb.sh a b {R}; echo still {A}", "sb 2\nstill\n"),
    ("env_script_rc", f"env smoke/sb3.sh > /dev/null 2>&1; echo $? {R}", "3\n"),
    ("env_script_pipe", f"env smoke/sb.sh a | cat {R}", "sb 1\n"),
    ("env_script_sub", f"x=$(env smoke/sb.sh a); echo \"got $x\" {R}", "got sb 1\n"),
    ("env_script_job", f"env smoke/sb.sh a {R} & wait $!; echo rc=$? {A}", "sb 1\nrc=0\n"),
    ("env_script_var", f"printf '#!/bin/exsh\\necho \"[$EV1]\"\\nenv | grep -c ^EV1=\\n' > smoke/ev1.sh; chmod +x smoke/ev1.sh; env EV1=1 smoke/ev1.sh {R}; echo \"[$EV1]\" {A}", "[1]\n1\n[]\n"),
    ("env_i_script", f"printf '#!/bin/exsh\\nenv | grep -c ^EV2=\\necho \"[$EV2]\"\\n' > smoke/ev4.sh; chmod +x smoke/ev4.sh; export EV2=1; env -i smoke/ev4.sh {R}; env | grep -c ^EV2= {A}; unset EV2", "0\n[]\n1\n"),
    ("env_chdir_script", f"mkdir -p smoke/ec; printf '#!/bin/exsh\\npwd\\n' > smoke/ec/p.sh; chmod +x smoke/ec/p.sh; env -C smoke/ec ./p.sh {R}; pwd {A}", "/home/smoke/ec\n/home\n"),
    ("env_frontend_file", f"env Examples/clike/base/hello {R}", "Hello World!\n"),
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
    # An SDL window gone as soon as it is up. It used to come back after the close, and released
    # while key it took the app down, so each row waits out the promote retries.
    ("sdl_open_close", f"pascal smoke/sdlclose.pas {R}; sleep 3", "closed\n"),
    ("sdl_open_error", f"pascal smoke/sdlerror.pas > /dev/null 2>&1; echo back {R}; sleep 3", "back\n"),
    ("gl_quad_strip", f"pascal smoke/glprims.pas {R}; sleep 3", "drawn\n"),
    ("applet_fail_rc", f"awk 1 smoke/nope > /dev/null 2>&1; echo $? {R}", "2\n"),
    ("shebang_fail_rc", f"smoke/sb3.sh > /dev/null 2>&1; echo $? {R}", "3\n"),
    # A #! frontend has no process exit to flush its output: it arrived after the
    # redirection was gone, or never.
    ("shebang_frontend_file", f"Examples/clike/base/hello {R}", "Hello World!\n"),
    ("shebang_frontend_sub", f"x=$(Examples/clike/base/hello); echo \"got $x\" {R}", "got Hello World!\n"),
    ("shebang_frontend_pipe", f"Examples/clike/base/hello | cat {R}", "Hello World!\n"),
    ("shebang_frontend_time", f"time Examples/clike/base/hello | head -1 {R}", "Hello World!\n"),
    # time is a keyword, so it times a function too, and the applet stays one quote away.
    ("time_function", f"tj() {{ echo in; return 3; }}; {{ {{ time tj; }} 2>/dev/null; echo $?; }} {R}", "in\n3\n"),
    ("time_applet_quoted", f"\\time Examples/clike/base/hello 2>/dev/null | head -1 {R}", "Hello World!\n"),
    # return in a trap's text ends the function it broke into, and a later eval does not end anything.
    ("return_err_trap", f"rt() {{ trap 'return 1' ERR; false; echo no; }}; {{ rt; echo $?; trap - ERR; eval 'echo later'; }} {R}", "1\nlater\n"),
    # This file is sourced, so a return here would end the whole run. Each shell or file here has its own.
    ("return_outside", f"exsh -c 'eval return 2>/dev/null; echo after' {R}", "after\n"),
    ("return_in_source", f"printf 'eval return\\necho no\\n' > smoke/rs.sh; {{ . smoke/rs.sh; echo after; }} {R}", "after\n"),
    ("exit_trap_redirected", f"( trap 'echo in-trap' EXIT; {{ exit 3; }} > smoke/et.txt; echo no ) {R}; echo \"rc=$?\" {A}; cat smoke/et.txt {A}", "rc=3\nin-trap\n"),
    # Slots used as host descriptors: fchdir left find in the first directory, and
    # fopen on a device read whatever host descriptor had the slot's number.
    ("find_execdir", f"find smoke/sub smoke/sub2 -type f -execdir pwd \\; {R}", "/home/smoke/sub\n/home/smoke/sub2\n"),
    ("wc_dev_tty", f"wc -c /dev/tty3 {R}", "0 /dev/tty3\n"),
    # fcntl, pread and pwrite had no shim: utmp records were read from and written to
    # whichever host descriptor had the slot's number, or nowhere if none did.
    ("who_file", f"who -q smoke/utmp.rec {R}", "alice bob carol\n# users=3\n"),
    ("users_file", f"users smoke/utmp.rec {R}", "alice bob carol\n"),
    # ttyname took a slot for a host descriptor, so nothing was this session's terminal.
    ("who_m", f"who -m smoke/utmp.rec {R}; cut -d' ' -f1 {{o}} > {{o}}.1; mv {{o}}.1 {{o}}", "carol\n"),
    ("who_m_not_tty", f"who -m smoke/utmp.rec < /dev/null {R}", ""),
    ("tty_named", f"tty {R}", "/dev/pts/0\n"),
    # Replacing /etc/passwd reloads the app's table while other commands read it, and that load had no lock.
    ("user_db_reload", f"w=$(whoami); i=0; while [ $i -lt 40 ]; do cp /etc/passwd /etc/passwd.t; mv /etc/passwd.t /etc/passwd; whoami > /dev/null & whoami > /dev/null & id > /dev/null & wait; i=$((i+1)); done; [ \"$(whoami)\" = \"$w\" ] && echo same {R}", "same\n"),
    ("uptime_users", f"mkdir -p /var/run; cp smoke/utmp.rec /var/run/utmp; uptime | grep -c ' 3 users, ' {R}; rm /var/run/utmp", "1\n"),
    # xargs reaches the applet by a call; the next two go through shell dispatch.
    ("wtmp_append", f"mkdir -p /var/log; rm -f /var/log/wtmp; xargs halt -w < /dev/null {R}; xargs halt -w < /dev/null {A}; wc -c < /var/log/wtmp {A}; who -r /var/log/wtmp | wc -l {A}", "800\n2\n"),
    ("wtmp_partial", f"printf x > /var/log/wtmp; xargs halt -w < /dev/null {R}; wc -c < /var/log/wtmp {A}", "400\n"),
    # These reached the VM's Halt, which takes one integer: "Halt expects 0 or 1 integer argument."
    ("halt_w", f"rm -f /var/log/wtmp; halt -w {R}; echo $? {A}; reboot -w {A}; echo $? {A}; poweroff -w {A}; echo $? {A}; wc -c < /var/log/wtmp {A}", "0\n0\n0\n1200\n"),
    ("halt_w_sourced", f"printf 'halt -w\\necho after\\n' > smoke/hw.sh; . smoke/hw.sh {R}", "after\n"),
    ("halt_bad_option", f"halt -z {R}; echo $? {A}", "halt: illegal option -- z\nusage: halt|poweroff|reboot [-dfhHinpw]\n1\n"),
    # And F_SETFL reached a host descriptor too: fd 0 from tail, a system directory
    # from stty. Only a trace shows that, so these say the paths still work; the shims
    # have their tests in Tests/ios_vproc.
    ("tail_f_pipe", f"cat smoke/d.txt | tail -f --pid=999999 {R}", "b\na\nc\n"),
    ("tail_f_stdin", f"tail -f --pid=999999 < /dev/null {R}; echo $? {A}", "0\n"),
    ("tail_f_device", f"tail -f --pid=999999 /dev/null {R}; echo $? {A}", "0\n"),
    ("tail_F_appears", f"tail -F -s 0.2 smoke/appear {R} & t=$!; sleep 0.5; ln -s /dev/null smoke/appear; i=0; while [ $i -lt 200 ] && ! grep -q appeared {{o}}; do sleep 0.1; i=$((i+1)); done; kill $t; wait $t 2>/dev/null", "tail: cannot open 'smoke/appear' for reading: No such file or directory\ntail: 'smoke/appear' has appeared;  following new file\n"),
    # fstat called a pipeline's pipe a character device, so tail followed it for ever,
    # and did not see that its own reader had gone.
    ("tail_f_stdin_pipe", f"cat smoke/d.txt | tail -f {R}; echo $? {A}", "b\na\nc\n0\n"),
    ("tail_f_reader_gone", f"tail -f smoke/d.txt | head -1 {R}", "b\n"),
    ("stat_stdin_pipe", f"echo x | stat -c %F - {R}", "fifo\n"),
    ("stty_named", f"stty -F /dev/tty size > /dev/null 2>&1; echo $? {R}", "0\n"),
    ("stty_not_tty", f"stty -F /dev/null {R}; echo $? {A}", "stty: /dev/null: Inappropriate ioctl for device\n1\n"),
    # pscal-core's own fcntl on 0-2 and on dup's slots reached the host's descriptors.
    # ReadKey set O_NONBLOCK on the host's fd 0, so a lone ESC on a pipe waited for the
    # rest of a sequence until the writer ended, where it should give up after 100 ms.
    ("readkey_lone_esc", f"s=$(date +%s); {{ cat smoke/esc.bin; sleep 8; }} | {{ pascal smoke/key.pas; e=$(date +%s); [ $((e - s)) -lt 6 ] && echo quick; }} {R}", "27\nquick\n"),
    ("readkey_after_esc", f"{{ cat smoke/esc.bin; sleep 1; printf x; }} | pascal smoke/key2.pas {R}", "27\n120\n"),
    # /proc/<pid>/fd listed the host's descriptors, whichever vproc asked and for
    # whichever pid. Numbers this high are slots only here: no host descriptor has them.
    ("proc_fd_own_slot", f"ls /proc/self/fd 201< smoke/d.txt | grep -x 201 {R}", "201\n"),
    # The sleep is the refresh's limit of once in 250 ms: the last test's refresh is that recent.
    ("proc_fd_other_vproc", f"exec 202< smoke/d.txt; sleep 0.3; ls /proc/$$/fd | grep -x 202 {R}; exec 202<&-", "202\n"),
    ("proc_fdinfo_slot", f"{{ read x; cat /proc/self/fdinfo/0; }} < smoke/d.txt {R}", "pos:\t2\nflags:\t00\nmnt_id:\t0\n"),
    # vi's F_FULLFSYNC after a write reached whichever host descriptor had the slot's
    # number and failed there, so the file never got one. Only a trace shows that.
    ("vi_write", f"EXINIT='w! smoke/vo.txt:q!' vi -s smoke/d.txt < /dev/null > /dev/null 2>&1; echo $? {R}; cat smoke/vo.txt {A}", "0\nb\na\nc\n"),
    # A socket's number is a host descriptor, so socketSetBlocking stays the host's fcntl.
    ("socket_set_blocking", f"pascal smoke/sock.pas {R}", "0\n0\n"),
    # A socket's number can also be a slot's. Poll then asked about the slot, a file that is
    # always ready, and close took the slot and left the socket open.
    ("socket_number_is_a_slot", f"exec 200>smoke/s200; pascal smoke/s1.pas {R}; echo still >&200; echo \"wr=$?\" {A}; exec 200>&-; echo \"file=$(cat smoke/s200)\" {A}", "200\n0\n0\n200\nwr=0\nfile=still\n"),
    ("socket_tcp", f"pascal smoke/tcp.pas {R}", "listen 0\nno client 0\nconnect 0\nclient waiting 1\nnothing sent 0\ncan write 2\nsend 5\ndata waiting 1\nreceived hello\ndrained 0\nclose 0\npeer gone 1\n"),
    ("socket_tcp_numbers_are_slots", f"i=200; while [ $i -le 207 ]; do eval \"exec $i>>smoke/s3\"; i=$((i+1)); done; FILL=1 pascal smoke/tcp.pas {R}; i=200; while [ $i -le 207 ]; do eval \"exec $i>&-\"; i=$((i+1)); done", "listen 0\nno client 0\nconnect 0\nclient waiting 1\nnothing sent 0\ncan write 2\nsend 5\ndata waiting 1\nreceived hello\ndrained 0\nclose 0\npeer gone 1\nnumbers 200\n"),
    # A stream can't write to stdout opened for reading. Its fallback dup'ed the host number
    # through the slot shim and kept what came back: four or five descriptors a command.
    ("read_only_stdout_keeps_nothing", f"pascal smoke/sno.pas > /dev/null; a=$(pascal smoke/sno.pas); i=0; while [ $i -lt 10 ]; do echo x 1<smoke/d.txt 2>/dev/null; pascal smoke/hello.pas 1<smoke/d.txt 2>/dev/null; i=$((i+1)); done; b=$(pascal smoke/sno.pas); if [ $((b-a)) -lt 10 ]; then echo ok {R}; else echo \"$a $b\" {R}; fi", "ok\n"),
    # A command under timeout is a child: a thread with a vproc of its own. A real fork left
    # the shim's waitpid with no child it knew: "timeout: waitpid: No child processes", 125.
    ("timeout_echo", f"timeout 5 echo hi {R}; echo $? {A}", "hi\n0\n"),
    ("timeout_status", f"timeout 5 false {R}; echo $? {A}", "1\n"),
    ("timeout_expires", f"timeout 0.3 sleep 30 {R}; echo $? {A}", "124\n"),
    ("timeout_zero", f"timeout 0 echo no-limit {R}; echo $? {A}", "no-limit\n0\n"),
    ("timeout_preserve", f"timeout --preserve-status 0.3 sleep 30 {R}; echo $? {A}", "143\n"),
    ("timeout_signal", f"timeout -s KILL --preserve-status 0.3 sleep 30 {R}; echo $? {A}", "137\n"),
    ("timeout_missing", f"timeout 5 no-such-command {R}; echo $? {A}", "timeout: no-such-command: No such file or directory\n127\n"),
    ("timeout_stdin", f"timeout 5 cat < smoke/d.txt {R}", "b\na\nc\n"),
    ("timeout_pipe", f"cat smoke/d.txt | timeout 5 tail -1 | cat {R}", "c\n"),
    ("timeout_cwd", f"(cd smoke/sub; timeout 5 pwd) {R}", "/home/smoke/sub\n"),
    ("timeout_env", f"SPAWNV=1 timeout 5 printenv SPAWNV {R}", "1\n"),
    ("timeout_subshell_env", f"(export SPAWNS=2; timeout 5 printenv SPAWNS) {R}; printenv SPAWNS {A}", "2\n"),
    ("timeout_frontend", f"timeout 5 pascal smoke/hello.pas {R}; echo $? {A}", "pascal ok 42\n0\n"),
    ("timeout_frontend_status", f"timeout 5 pascal smoke/halt.pas > /dev/null 2>&1; echo $? {R}", "3\n"),
    ("timeout_script", f"timeout 5 smoke/sb3.sh; echo $? {R}", "3\n"),
    ("timeout_shell", f"timeout 5 sh -c 'echo in-sh; exit 4' {R}; echo $? {A}", "in-sh\n4\n"),
    ("timeout_keeps_shell", f"timeout 5 sh -c 'SPAWNK=1; cd smoke; exit 0'; echo \"[$SPAWNK]\" {R}; ls -d smoke {A}", "[]\nsmoke\n"),
    # A kill cancels the command's thread wherever it waits. Left with the lock held, the
    # pipe's other end, the terminal or every later command would wait for ever.
    ("timeout_empty_pipe", f"sleep 1 | timeout 0.3 cat {R}; echo $? {A}; echo after | cat {A}", "124\nafter\n"),
    ("timeout_full_pipe", f"timeout 0.3 yes | sleep 1 {R}; echo $? {A}; echo after | cat {A}", "0\nafter\n"),
    ("timeout_reader_gone", f"yes | timeout 0.3 cat > /dev/null; echo $? {R}; echo after | cat {A}", "124\nafter\n"),
    ("timeout_terminal", f"timeout 0.3 cat {R}; echo $? {A}; stty size > /dev/null 2>&1; echo $? {A}; echo after | cat {A}", "124\n0\nafter\n"),
    ("timeout_nested_shell", f"timeout 0.3 sh -c 'sleep 30; echo late'; echo $? {R}; sleep 0.2; echo after | cat {A}", "124\nafter\n"),
    ("timeout_killed", f"timeout 30 sleep 30 & t=$!; sleep 0.3; kill $t; wait $t; echo $? {R}; echo after | cat {A}", "143\nafter\n"),
    # What the killed shell was running must go with it: its descriptors keep the reader waiting.
    ("timeout_nested_reader", f"(timeout 0.3 sh -c 'sleep 30; echo late') | cat {R}; echo $? {A}", "0\n"),
    ("timeout_nested_pipeline", f"timeout 0.3 sh -c 'sleep 30 | cat; echo late' | cat {R}; echo $? {A}; lps | grep -c '[s]leep 30' {A}", "0\n0\n"),
    # A loop that makes no calls stops too. Left running it keeps a core busy until the app quits,
    # and it holds stdout, so the reader never finishes.
    ("timeout_vm_loop", f"timeout 1 pascal smoke/spin.pas | cat {R}; echo $? {A}; echo after | cat {A}", "0\nafter\n"),
    # A shell run as a command is no prompt, and says nothing about the one that ran it, running,
    # killed or not. A prompt told it was a script gave commands no terminal, so `awk < f` read nothing.
    ("sh_c_not_prompt", f"sh -c 'case $- in *i*) echo prompt;; *) echo script;; esac' {R}", "script\n"),
    ("stage_is_prompt", f"{{ case $- in *i*) echo prompt;; *) echo script;; esac; }} | cat {R}", "prompt\n"),
    ("nested_bg_prompt", f"sh -c 'sleep 0.5' & sleep 0.2; case $- in *i*) echo prompt {R};; *) echo script {R};; esac; wait", "prompt\n"),
    ("nested_killed_prompt", f"sh -c 'sleep 30' & t=$!; sleep 0.2; kill $t; wait $t; case $- in *i*) echo prompt {R};; *) echo script {R};; esac", "prompt\n"),
    ("timeout_nested_prompt", f"timeout 0.3 sh -c 'sleep 30'; case $- in *i*) echo prompt {R};; *) echo script {R};; esac; awk '{{print $1}}' < smoke/d.txt {A}", "prompt\nb\na\nc\n"),
    ("xargs_frontend", f"echo smoke/hello.pas | xargs pascal {R}; echo $? {A}", "pascal ok 42\n0\n"),
    ("xargs_missing", f"echo x | xargs no-such-command {R}; echo $? {A}", "xargs: no-such-command: No such file or directory\n127\n"),
    ("find_exec_frontend", f"find smoke/hello.pas -exec pascal {{}} \\; {R}", "pascal ok 42\n"),
    # A missing directory is an error, as in bash: nothing makes it on the way.
    ("redirect_no_dir", f"echo x > /nonexistent/dir/f; echo $? {R}; ls /nonexistent {A}", "1\nls: cannot access '/nonexistent': No such file or directory\n"),
    ("touch_no_dir", f"touch /nonexistent/dir/f > /dev/null 2>&1; echo $? {R}; ls /nonexistent {A}", "1\nls: cannot access '/nonexistent': No such file or directory\n"),
    ("cp_no_dir", f"cp smoke/d.txt /nonexistent/dir/f > /dev/null 2>&1; echo $? {R}; ls /nonexistent {A}", "1\nls: cannot access '/nonexistent': No such file or directory\n"),
    # A VM program's stdin is the file or pipe it was given: waiting for the keyboard here
    # stalls the run. Keyboard input and Ctrl-C need a real prompt, which this tool does not
    # reach. The expected output is what a host build prints.
    ("stdin_file", f"pascal smoke/fp < smoke/ten {R}", PRIMES),
    ("stdin_pipe", f"printf '10\\n' | pascal smoke/fp {R}", PRIMES),
    ("stdin_shebang_file", f"smoke/fp < smoke/ten {R}", PRIMES),
    ("stdin_shebang_pipe", f"printf '10\\n' | smoke/fp {R}", PRIMES),
    ("stdin_shebang_twice", f"smoke/fp < smoke/ten {R}; smoke/fp < smoke/ten {A}", PRIMES + PRIMES),
    ("stdin_twice", f"pascal smoke/rl1.pas < smoke/d.txt {R}; pascal smoke/rl1.pas < smoke/u.txt {A}", "got b\ngot a\n"),
    ("stdin_dev_null", f"pascal smoke/rl1.pas < /dev/null {R}", "got \n"),
    ("stdin_lines", f"pascal smoke/rl3.pas < smoke/d.txt {R}", "[b][a][c]\n"),
    ("stdin_no_last_newline", f"printf 'x\\ny' | pascal smoke/rl3.pas {R}", "[x][y][]\n"),
    ("stdin_cr", f"printf 'a\\r\\nb\\nc\\n' | pascal smoke/rl3.pas {R}", "[a][b][c]\n"),
    ("stdin_empty_lines", f"printf '\\n\\nx\\n' | pascal smoke/rl3.pas {R}", "[][][x]\n"),
    # Only the terminal's input has control keys. Elsewhere ^C is text.
    ("stdin_control_text", f"pascal smoke/rl3.pas < smoke/ctl.txt {R}", "[^cat][^Zoo][a\x03b]\n"),
    ("stdin_control_pipe", f"cat smoke/ctl.txt | pascal smoke/rl3.pas {R}", "[^cat][^Zoo][a\x03b]\n"),
    # Read and Readln share a buffer, and a program's own file is not its stdin.
    ("stdin_read_mixed", f"printf '12 30 rest of line\\nQnext\\n' | pascal smoke/rdmix.pas {R}", "42|rest of line|Q|next\n"),
    ("stdin_own_file", f"pascal smoke/rlown.pas < smoke/d.txt {R}", "bba\n"),
    ("stdin_large_pipe", f"{{ seq 1 50000; echo end; }} | pascal smoke/rlcount.pas {R}", "50000 50000\n"),
    ("stdin_large_file", f"{{ seq 1 50000; echo end; }} > smoke/big; pascal smoke/rlcount.pas < smoke/big {R}", "50000 50000\n"),
    # Two VMs: Gen's ioresult needs the lock that Eof holds, and when Gen ends its fflush(NULL)
    # needs the lock of the FILE the reader is reading. Neither may be held while waiting for Gen.
    # Different languages: two of one language compiling at once is a Known Issue.
    ("stdin_vm_to_vm", f"pascal smoke/gen.pas | aether smoke/all.aether {R}", "line 1\nline 2\nline 3\nend\n"),
    ("stdin_vm_to_vm_eof", f"pascal smoke/gen.pas | clike smoke/rlgen.clike {R}", "4 end\n"),
    ("stdin_vm_to_vm_short", f"pascal smoke/hello.pas | clike smoke/rlgen.clike {R}", "1 pascal ok 42\n"),
    ("stdin_heredoc", "pascal smoke/rl3.pas > {o} 2>&1 <<EOF\nh1\nh2\nh3\nEOF", "[h1][h2][h3]\n"),
    ("stdin_here_string", f"pascal smoke/rl1.pas <<< hs {R}", "got hs\n"),
    ("stdin_group", f"{{ pascal smoke/rl1.pas; }} < smoke/d.txt {R}", "got b\n"),
    ("stdin_subshell", f"( pascal smoke/rl1.pas ) < smoke/d.txt {R}", "got b\n"),
    ("stdin_pipe_into_group", f"cat smoke/d.txt | {{ pascal smoke/rl3.pas; }} {R}", "[b][a][c]\n"),
    ("stdin_script", f"sh smoke/rds.sh < smoke/d.txt {R}", "got b\n"),
    # Only its stdout: a background frontend's stderr has debug lines from the shell.
    ("stdin_background", "pascal smoke/rl1.pas < smoke/d.txt > {o} 2> /dev/null & wait", "got b\n"),
    ("stdin_loop_body", f"while read l; do echo $l | pascal smoke/rl1.pas {A}; done < smoke/d.txt", "got b\ngot a\ngot c\n"),
    ("stdin_clike_file", f"clike smoke/sc.clike < smoke/num {R}", "clike got 42\n"),
    ("stdin_clike_pipe", f"printf '21\\n' | clike smoke/sc.clike {R}", "clike got 42\n"),
    ("stdin_clike_eof_file", f"clike smoke/rlall.clike < smoke/d.txt {R}", "[b]\n[a]\n[c]\n"),
    ("stdin_clike_eof_pipe", f"printf 'x\\ny' | clike smoke/rlall.clike {R}", "[x]\n[y]\n"),
    ("stdin_clike_eof_scanf", f"clike smoke/rlmix.clike < smoke/d.txt {R}", "[b]\n[a]\n[c]\n"),
    ("stdin_clike_large", f"seq 1 50000 | clike smoke/rlgen.clike {R}", "50000 50000\n"),
    ("stdin_rea_pipe", f"printf 'x y\\n' | rea smoke/rl.rea {R}", "rea got x y\n"),
    ("stdin_rea_eof", f"rea smoke/rlall.rea < smoke/d.txt {R}", "[ b]\n[ a]\n[ c]\n"),
    ("stdin_aether_pipe", f"printf 'p\\nq\\n' | aether smoke/rl.aether {R}", "aether got p\nthen q\n"),
    ("stdin_aether_end", f"aether smoke/rl.aether < /dev/null {R}", "aether got \nthen \n"),
    ("stdin_aether_all", f"aether smoke/all.aether < smoke/d.txt {R}", "b\na\nc\n"),
    ("stdin_aether_all_large", f"seq 1 50000 | aether smoke/all.aether | tail -1 {R}", "50000\n"),
    # A kill of the job used to end the app in the readln form, and the eof form stopped every VM.
    ("stdin_killed_readln", f'sleep 30 | clike smoke/rl1.clike > /dev/null & j=$!; sleep 0.5; kill $j; wait $j; echo "rc $?" {R}; clike smoke/sc.clike < smoke/num {A}', "rc 143\nclike got 42\n"),
    ("stdin_killed_pascal", f'sleep 30 | pascal smoke/rl1.pas > /dev/null & j=$!; sleep 0.5; kill $j; wait $j; echo "rc $?" {R}; clike smoke/sc.clike < smoke/num {A}', "rc 143\nclike got 42\n"),
    ("stdin_killed_eof", f'sleep 30 | clike smoke/rlall.clike > /dev/null & j=$!; sleep 0.5; kill $j; wait $j; echo "rc $?" {R}; clike smoke/sc.clike < smoke/num {A}', "rc 143\nclike got 42\n"),
    ("stdin_others_run_while_eof_waits", f'tail -f smoke/d.txt | clike smoke/rlall.clike > /dev/null & j=$!; sleep 0.5; x=$(echo sub); echo "$x" {R}; clike smoke/sc.clike < smoke/num {A}; kill $j; wait $j; echo "rc $?" {A}; unset x', "sub\nclike got 42\nrc 143\n"),
    ("tr_cat", f"cat smoke/d.txt | tr a-z A-Z {R}", "B\nA\nC\n"),
    ("awk_cat", f"cat smoke/d.txt | awk '{{print $1}}' {R}", "b\na\nc\n"),
    ("awk_lt", f"awk '{{print $1}}' < smoke/d.txt {R}", "b\na\nc\n"),
    # Only the terminal stops a background reader. These stopped for good, or read nothing.
    ("bg_awk_lt", f"awk '{{print $1}}' < smoke/d.txt {R} & wait $!; echo \"rc=$?\" {A}", "b\na\nc\nrc=0\n"),
    ("bg_cat_lt", f"cat < smoke/d.txt {R} & wait $!; echo \"rc=$?\" {A}", "b\na\nc\nrc=0\n"),
    ("bg_group_lt", f"{{ cat; }} < smoke/d.txt {R} & wait $!; echo \"rc=$?\" {A}", "b\na\nc\nrc=0\n"),
    ("bg_read_lt", f"read x < smoke/d.txt & wait $!; echo \"rc=$?\" {R}", "rc=0\n"),
    ("bg_sort_lt", f"sort < smoke/d.txt {R} & wait $!; echo \"rc=$?\" {A}", "a\nb\nc\nrc=0\n"),
    ("bg_script_lt", f"printf '#!/bin/exsh\\ncat\\nexit 4\\n' > smoke/bgl.sh; chmod +x smoke/bgl.sh; smoke/bgl.sh < smoke/d.txt {R} & wait $!; echo \"rc=$?\" {A}", "b\na\nc\nrc=4\n"),
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
    # ended, so a command that wrote more than a pipe holds never ended.
    ("sub_big_applet", f'x=$(seq 1 30000); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_backtick", f'x=`seq 1 30000`; echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_last", f'echo "$(seq 1 30000)" | tail -1 {R}', "30000\n"),
    ("sub_big_builtins", f'x=$(i=0; while [ "$i" -lt 8000 ]; do echo "line number $i"; i=$((i+1)); done); echo "${{#x}}" {R}', "134889\n"),
    ("sub_big_echo", f'x=$(seq 1 30000); y=$(echo "$x"; echo "$x"); echo "${{#y}}" {R}', "337787\n"),
    ("sub_big_pipeline", f'x=$(seq 1 30000 | cat); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_pipeline3", f'x=$(seq 1 30000 | cat | cat); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_group", f'x=$({{ seq 1 30000; echo end; }}); echo "${{#x}}" {R}', "168897\n"),
    ("sub_big_subshell", f'x=$( (seq 1 30000) ); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_loop", f'x=$(for i in 1 2; do seq 1 30000; done); echo "${{#x}}" {R}', "337787\n"),
    ("sub_big_function", f'bigf() {{ seq 1 30000; }}; x=$(bigf); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_nested", f'x=$(echo "$(seq 1 30000)"); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_file", f'seq 1 30000 > smoke/big.txt; x=$(cat smoke/big.txt); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_stdin", f'x=$(cat < smoke/big.txt); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_status", f'x=$(seq 1 30000; exit 3); echo "$? ${{#x}}" {R}', "3 168893\n"),
    ("sub_big_pascal", f'x=$(pascal smoke/bigout.pas); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_clike", f'x=$(clike smoke/bigout.clike); echo "${{#x}}" {R}', "168893\n"),
    ("sub_big_sh_c", f"x=$(sh -c 'seq 1 30000'); echo \"${{#x}}\" {R}", "168893\n"),
    ("sub_big_in_sh_c", f"sh -c 'x=$(seq 1 30000); echo ${{#x}}' {R}", "168893\n"),
    ("sub_big_script", f"printf 'x=$(seq 1 30000)\\necho ${{#x}}\\n' > smoke/bs.sh; exsh smoke/bs.sh {R}", "168893\n"),
    ("sub_big_shebang", f"printf '#!/bin/exsh\\nx=$(seq 1 30000)\\necho ${{#x}} {R}\\n' > smoke/bs2.sh; chmod +x smoke/bs2.sh; smoke/bs2.sh", "168893\n"),
    ("sub_big_in_subshell", f'( x=$(seq 1 30000); echo "${{#x}}" {R} )', "168893\n"),
    ("sub_big_in_stage", f'echo a | {{ x=$(seq 1 30000); echo "${{#x}}"; }} {R}', "168893\n"),
    ("sub_big_background", f'( x=$(seq 1 30000); echo "${{#x}}" > smoke/bgb.txt ) & wait $!; cat smoke/bgb.txt {R}', "168893\n"),
    ("sub_big_twenty", f'n=0; s=0; while [ "$n" -lt 20 ]; do x=$(seq 1 30000); s=$((s+${{#x}})); n=$((n+1)); done; echo "$s" {R}', "3377860\n"),
    # A program sees what is exported and nothing else. Stages are threads of one process here,
    # so each program gets an environment of its own; x above is still set and still big.
    ("big_var_program", f"seq 1 1 {R}", "1\n"),
    ("big_var_not_passed", f"env | grep -c '^x=' {R}", "0\n"),
    ("big_var_sh", f"sh -c 'echo \"[${{#x}}]\"' {R}", "[0]\n"),
    ("big_var_stage", f"env | grep -c '^x=' | cat {R}", "0\n"),
    ("export_plain", f"ev=1; env | grep -c '^ev=' {R}", "0\n"),
    ("export_plain_sh", f"sh -c 'echo \"[$ev]\"' {R}", "[]\n"),
    ("export_exported", f"export ew=2; env | grep '^ew=' {R}", "ew=2\n"),
    ("export_exported_sh", f"sh -c 'echo \"[$ew]\"' {R}", "[2]\n"),
    ("export_n", f"export -n ew; env | grep -c '^ew=' {R}", "0\n"),
    ("export_n_keeps_value", f"echo \"[$ew]\" {R}", "[2]\n"),
    ("export_declare_x", f"declare -x ex=3; env | grep '^ex=' {R}", "ex=3\n"),
    ("export_declare_plus_x", f"declare +x ex; env | grep -c '^ex=' {R}", "0\n"),
    ("export_subshell_sees", f"( echo \"[$ev]\"; env | grep -c '^ev=' ) {R}", "[1]\n0\n"),
    ("export_stage_sees", f"{{ echo \"[$ev]\"; env | grep -c '^ev='; }} | cat {R}", "[1]\n0\n"),
    ("export_substitution_sees", f"y=$(echo \"[$ev]\"; env | grep -c '^ev='); echo \"$y\" {R}", "[1]\n0\n"),
    ("export_in_subshell_stays", f"( export ev; env | grep '^ev=' ) {R}; env | grep -c '^ev=' {A}", "ev=1\n0\n"),
    ("export_in_substitution_stays", f"y=$(export ev; env | grep '^ev='); echo \"$y\" {R}; env | grep -c '^ev=' {A}", "ev=1\n0\n"),
    ("export_local", f"lf() {{ local -x lv=1; env | grep '^lv='; sh -c 'echo \"[$lv]\"'; }}; lf {R}; env | grep -c '^lv=' {A}", "lv=1\n[1]\n0\n"),
    ("export_local_in_substitution", f"lg() {{ local lv=2; y=$(echo \"[$lv]\"); echo \"$y\"; }}; lg {R}", "[2]\n"),
    ("prefix_program", f"ep=1 env | grep '^ep=' {R}; echo \"[$ep]\" {A}", "ep=1\n[]\n"),
    ("prefix_sh", f"ep=1 sh -c 'echo \"[$ep]\"' {R}", "[1]\n"),
    ("prefix_first_stage", f"ep=1 sh -c 'echo \"a[$ep]\"' | sh -c 'cat; echo \"b[$ep]\"' {R}", "a[1]\nb[]\n"),
    ("prefix_last_stage", f"sh -c 'echo \"a[$ep]\"' | ep=1 sh -c 'cat; echo \"b[$ep]\"' {R}", "a[]\nb[1]\n"),
    ("prefix_both_stages", f"ep=1 sh -c 'echo \"a[$ep][$eq]\"' | eq=2 sh -c 'cat; echo \"b[$ep][$eq]\"' {R}", "a[1][]\nb[][2]\n"),
    ("prefix_builtin_stage", f"ep=1 declare -p ep | cat {R}; echo \"[$ep]\" {A}", None),
    ("prefix_function", f"pf() {{ env | grep '^ep='; }}; ep=1 pf {R}; echo \"[$ep]\" {A}", "ep=1\n[]\n"),
    ("prefix_eval", f"ep=1 eval 'sh -c \"echo [\\$ep]\"' {R}; echo \"[$ep]\" {A}", "[1]\n[]\n"),
    ("prefix_command", f"ep=1 command env | grep '^ep=' {R}", "ep=1\n"),
    ("prefix_background", f"ep=1 sh -c 'echo \"[$ep]\"' > smoke/pb.txt & wait $!; cat smoke/pb.txt {R}", "[1]\n"),
    ("prefix_applet_stage", f"ep=1 env | grep -c '^ep=' | cat {R}", "1\n"),
    ("prefix_frontend", f"ep=1 clike smoke/envp.clike {R}", "[1]\n"),
    ("prefix_frontend_stage", f"ep=1 clike smoke/envp.clike | cat {R}", "[1]\n"),
    ("frontend_exported", f"export ep=2; clike smoke/envp.clike {R}; unset ep", "[2]\n"),
    ("frontend_not_exported", f"ep=3; clike smoke/envp.clike {R}; unset ep", "[]\n"),
    ("frontend_pascal_prefix", f"ep=4 pascal smoke/envp.pas {R}", "[4]\n"),
    ("script_plain", f"printf 'echo \"[$ev][$ew]\"\\n' > smoke/ns.sh; chmod +x smoke/ns.sh; export ew; smoke/ns.sh {R}; export -n ew", "[][2]\n"),
    ("script_shebang", f"printf '#!/bin/exsh\\necho \"[$ev][$ew]\"\\n' > smoke/ss.sh; chmod +x smoke/ss.sh; export ew; smoke/ss.sh {R}; export -n ew", "[][2]\n"),
    ("script_prefix", f"ep=5 smoke/ns.sh {R}", None),
    ("script_stays", f"printf 'ev=9; export es=1\\n' > smoke/ch.sh; chmod +x smoke/ch.sh; smoke/ch.sh; echo \"[$ev][$es]\" {R}", "[1][]\n"),
    ("sh_c_stays", f"sh -c 'ev=9; export es=1'; echo \"[$ev][$es]\" {R}", "[1][]\n"),
    ("sh_c_array", f"ea=(1 2); sh -c 'echo \"[$ea]\"' {R}", "[]\n"),
    ("sh_c_export_n", f"export ew; sh -c 'export -n ew; env | grep -c \"^ew=\"'; env | grep '^ew=' {R}; export -n ew", None),
    ("env_u_stays", f"env -u HOME env | grep -c '^HOME=' {R}; [ -n \"$HOME\" ] && echo kept {A}", "0\nkept\n"),
    ("env_set_stays", f"env ec=1 env | grep '^ec=' {R}; echo \"[$ec]\" {A}", "ec=1\n[]\n"),
    ("xargs_sees", f"ep=6 xargs env < /dev/null | grep '^ep=' {R}", None),
    ("find_exec_sees", f"ep=7 find smoke/d.txt -exec env \\; | grep '^ep=' {R}", None),
    ("sub_many_small", f'n=0; while [ "$n" -lt 300 ]; do v=$(echo "$n"); n=$((n+1)); done; echo "$v" {R}', "299\n"),
    ("sub_fd3_exec", f'x=$(exec 3> smoke/s3; echo out); echo "[$x]" {R}; unset x', "[out]\n"),
    ("sub_fd3_cmd", f'x=$(echo hi 3> smoke/s3); echo "[$x]" {R}; unset x', "[hi]\n"),
    ("sub_bg_holds", f"printf 'x=$(sleep 1 & echo bg)\\necho \"[$x]\"\\n' > smoke/bh.sh; exsh smoke/bh.sh {R}", "[bg]\n"),
    # In here the shell counts as interactive, and bash prints no job number in a substitution.
    ("sub_bg_no_job_message", f'x=$(sleep 0.1 & echo bg); echo "[$x]" {R}; unset x', "[bg]\n"),
    ("sub_late_output", f'z=$( {{ sleep 0.2; echo late; }} & echo early ); echo "[$z]" {R}; unset z', "[early\nlate]\n"),
    ("sub_job_gone_after", f'x=$(sleep 0.1 & echo bg); jobs {R}; wait; echo "rc $?" {A}; unset x', "rc 0\n"),
    ("sub_bg_elsewhere", f's=$(date +%s); x=$(sleep 5 > /dev/null 2>&1 & echo early); e=$(date +%s); [ "$((e-s))" -lt 3 ] && echo "quick [$x]" {R}; unset s e x', "quick [early]\n"),
    ("sub_job_ends_during", f'true & x=$(echo e); wait; echo "rc $? [$x]" {R}; unset x', "rc 0 [e]\n"),
    ("sub_exec", f'x=$(exec echo hi; echo no); echo "[$x]" {R}; unset x', "[hi]\n"),
    ("sub_exec_status", f'x=$(exec false); echo "rc $?" {R}; unset x', "rc 1\n"),
    ("sub_exec_function", f'smf() {{ exec echo in-f; echo no; }}; x=$(smf); echo "[$x]" {R}; unset x; unset -f smf', "[in-f]\n"),
    ("sub_exec_fd_ends", f'x=$(exec 7>&1; echo out); echo "[$x]" {R}; echo probe 2>/dev/null >&7; echo "rc $?" {A}; unset x', "[out]\nrc 1\n"),
    ("sub_exec_away", f'x=$(exec > /dev/null; echo gone); echo "[$x]" {R}; unset x', "[]\n"),
    ("subshell_exec", f'( exec echo in-sub; echo no ) {R}', "in-sub\n"),
    ("printf_wide_number", f'x=$(printf "%300d|" 7); echo "${{#x}}" {R}; unset x', "301\n"),
    ("printf_wide_char", f'x=$(printf "%300c|" a); echo "${{#x}}" {R}; unset x', "301\n"),
    # %n has no conversion in libc behind it: Darwin's aborted, which ended the whole app.
    ("printf_count", f'printf "ab%n|\\n" pn {R}; echo "[$pn] $?" {A}; unset pn', "ab|\n[2] 0\n"),
    ("printf_count_command", f'command printf "abc%n|" pn {R}; echo "[$pn]" {A}; unset pn', "abc|[3]\n"),
    ("printf_count_substitution", f'x=$(printf "abc%nd" pn); echo "[$x]" {R}; unset x', "[abcd]\n"),
    ("printf_count_bad_name", f'printf "%n" 1a 2>/dev/null; echo "$?" {R}', "1\n"),
    ("printf_unknown_letter", f'{{ printf "cd%Dgh" abc 2>/dev/null; echo " $?"; }} {R}', "cd 1\n"),
    # Killing a body that is inside a substitution leaves that substitution's reader with no
    # one to join it. The next substitution still has to work.
    ("sub_body_killed", f'( x=$(sleep 30); echo no ) & p=$!; sleep 1; kill $p; wait $p; s=$?; y=$(echo next); echo "$s $y" {R}; unset s y', "143 next\n"),
    # `kill` cancels a job's thread wherever it is: wait's status has to survive that, and so
    # does everything the thread held.
    ("body_killed", f'( sleep 30; echo no ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    ("body_killed_at_once", f'( sleep 30; echo no ) & p=$!; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    ("body_killed_sub_at_once", f'( x=$(sleep 30); echo no ) & p=$!; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    ("body_killed_in_wait", f'( sleep 30 & wait; echo no ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    ("body_killed_busy", f'( while :; do echo hi > /dev/null; done ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    ("body_killed_busy_sub", f'( while :; do x=$(echo hi); done ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    ("body_killed_pipe", f'( sleep 30 | cat; echo no ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    ("body_killed_reading", f'( read x; echo no ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    # These two print a job notification into their own output, which is not what they check.
    ("body_killed_in_subshell", '( ( sleep 30 ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" > {o} ) > /dev/null 2>&1', "rc 143\n"),
    ("body_killed_in_stage", f'{{ ( sleep 30 ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?"; }} | grep rc {R}', "rc 143\n"),
    ("simple_killed_at_once", f'sleep 30 & p=$!; kill $p; wait $p; echo "rc $?" {R}', "rc 143\n"),
    # wait finds any job that has ended, in any order and as often as it is asked.
    ("wait_any_job", f'( exit 3 ) & a=$!; ( exit 4 ) & b=$!; sleep 0.3; wait $a; echo "a $?" {R}; wait $b; echo "b $?" {A}; wait $a; echo "a $?" {A}', "a 3\nb 4\na 3\n"),
    ("wait_two_killed", f'( sleep 37 ) & a=$!; ( sleep 37 ) & b=$!; sleep 0.2; kill $a $b; wait $a; echo "a $?" {R}; wait $b; echo "b $?" {A}', "a 143\nb 143\n"),
    ("wait_several", f'( exit 3 ) & a=$!; ( exit 4 ) & b=$!; sleep 0.3; wait $b $a; echo "$?" {R}; wait; echo "$?" {A}', "3\n0\n"),
    # What the killed job was running goes with it: a shell inside it, and what that shell ran.
    ("body_killed_nested_shell", f'( sh -c "sleep 37; echo no" ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?" {R}; sleep 0.2; lps | grep "[s]leep 37" {A}', "rc 143\n"),
    ("body_killed_nested_reader", f'{{ ( sh -c "sleep 37; echo no" ) & p=$!; sleep 0.2; kill $p; wait $p; echo "rc $?"; }} | grep rc {R}', "rc 143\n"),
    # A loop that makes no calls stops too. It holds stdout, so the reader would never finish.
    ("kill_vm_loop", f"( pascal smoke/spin.pas & p=$!; sleep 1; kill $p; wait $p; echo $? ) | grep -v '^\\[' {R}; echo after | cat {A}", "143\nafter\n"),
    # [s]: grep's own row holds its argument, and an empty result shows any row that is left.
    ("killed_leaves_no_rows", f'( sleep 37 ) & p=$!; sleep 0.2; kill $p; wait $p; sleep 37 & p=$!; kill $p; wait $p; lps | grep "[s]leep 37" {R}', ""),
    # A trap is set on the shell's own task, not the command's, and runs on the shell's thread.
    ("trap_beside_job", f"trap 'echo got {A}' USR2; ( while :; do :; done ) & j=$!; sleep 0.2; kill -USR2 $$; sleep 0.2; echo end {A}; kill $j; wait $j 2>/dev/null; trap - USR2; unset j", "got\nend\n"),
    ("trap_from_job", f"trap 'echo got {A}' USR2; me=$$; ( sleep 0.1; kill -USR2 $me ) & i=0; while [ $i -lt 40 ]; do ( : ); sleep 0.01; i=$((i+1)); done; wait; echo end {A}; trap - USR2; unset i me", "got\nend\n"),
    ("trap_during_wait", f"trap 'echo got {A}' USR2; sleep 2 & j=$!; me=$$; ( sleep 0.3; kill -USR2 $me ) & wait $j; r=$?; [ $r -gt 128 ] && echo interrupted {A}; kill $j; wait 2>/dev/null; trap - USR2; unset j me r", "got\ninterrupted\n"),
    ("trap_cont_handler", f"( trap 'echo c {A}' CONT; sleep 1; echo body-end {A} ) & p=$!; sleep 0.3; kill -STOP $p; sleep 0.2; kill -CONT $p; wait $p; echo \"rc $?\" {A}; unset p", "c\nbody-end\nrc 0\n"),
    ("trap_job_keeps_none", f"trap 'echo shell {A}' TERM; sleep 30 & p=$!; kill $p; wait $p; echo \"rc $?\" {A}; trap - TERM; unset p", "rc 143\n"),
    ("trap_job_own", f"( trap 'echo term {A}; exit 3' TERM; while :; do sleep 0.01; done ) & p=$!; sleep 0.3; kill $p; wait $p; echo \"rc $?\" {A}; unset p", "term\nrc 3\n"),
    ("trap_nested_sh", f"sh -c 'trap \"echo got\" USR1; me=$$; ( sleep 0.1; kill -USR1 $me ) & sleep 0.3; echo end' {R}", "got\nend\n"),
    ("kill_nested_sh_from_job", f"sh -c 'me=$$; ( sleep 0.1; kill $me ) & sleep 0.3; echo alive' {R}; echo \"rc $?\" {A}", "rc 143\n"),
    # A `.` is no process, and a nested shell is: who a command's parent is.
    ("ppid_sourced", f"printf 'sh -c \"echo \\\\$PPID\" > smoke/pp\\n' > smoke/pp.sh; . smoke/pp.sh; [ \"$(cat smoke/pp)\" = \"$$\" ] && echo same {R}", "same\n"),
    ("ppid_nested_stage", f"printf 'echo $$ > smoke/pn1\\nsh -c \"echo \\\\$PPID\" | cat > smoke/pn2\\n' > smoke/pn.sh; sh smoke/pn.sh; [ \"$(cat smoke/pn1)\" = \"$(cat smoke/pn2)\" ] && echo same {R}", "same\n"),
    # A substitution puts back the stdout it found. A body's redirection moves only its slot,
    # and the substitution rebuilt that slot from the host's fd 1, so what followed it left
    # the redirection and went to the real stdout. Run in the app, a `#!` script lost it too.
    ("sub_keeps_subshell", f'( x=$(echo a); echo "$x" ) {R}; unset x', "a\n"),
    ("sub_keeps_around", f'( echo before; x=$(echo a); echo "$x"; y=$(echo b); echo "$y" ) {R}', "before\na\nb\n"),
    ("sub_keeps_if", f'if true; then x=$(echo a); echo "$x"; fi {R}; unset x', "a\n"),
    ("sub_keeps_for", f'for i in 1 2; do x=$(echo "$i"); echo "$x"; done {R}; unset x', "1\n2\n"),
    ("sub_keeps_group", f'{{ x=$(echo a); echo "$x"; }} {R}; unset x', "a\n"),
    ("sub_keeps_function", f'kf() {{ x=$(echo a); echo "$x"; }}; kf {R}; ( kf ) {A}; unset x; unset -f kf', "a\na\n"),
    ("sub_keeps_exec", f'( exec {R}; x=$(echo a); echo "$x"; y=$(echo b); echo "$y" )', "a\nb\n"),
    ("sub_keeps_exec_inside", f'( x=$(exec > smoke/kg; echo inner); echo "[$x]"; cat smoke/kg ) {R}', "[]\ninner\n"),
    ("sub_keeps_stage", f'( x=$(echo a); echo "$x" ) | {{ y=$(cat); echo "[$y]"; }} {R}', "[a]\n"),
    ("sub_keeps_applet", f'( x=$(echo a); ls smoke/d.txt ) {R}; unset x', "smoke/d.txt\n"),
    ("sub_keeps_frontend", f'( x=$(echo a); pascal smoke/hello.pas ) {R}; unset x', "pascal ok 42\n"),
    ("sub_keeps_many", f'( n=0; while [ "$n" -lt 200 ]; do x=$(echo "$n"); n=$((n+1)); done; echo "$x" ) {R}', "199\n"),
    ("sub_keeps_big", f'( x=$(seq 1 30000); echo "${{#x}}" ) {R}', "168893\n"),
    ("sub_keeps_shebang", f"printf '#!/bin/exsh\\nx=$(echo a)\\necho \"$x\"\\ny=$(echo b)\\necho \"$y\"\\n' > smoke/ks.sh; chmod +x smoke/ks.sh; smoke/ks.sh {R}", "a\nb\n"),
    ("sub_keeps_shebang_big", f"printf '#!/bin/exsh\\nx=$(seq 1 30000)\\necho ${{#x}}\\n' > smoke/ks2.sh; chmod +x smoke/ks2.sh; smoke/ks2.sh {R}", "168893\n"),
    ("sub_keeps_exsh_script", f"printf 'x=$(echo a)\\necho \"$x\"\\n' > smoke/ks3.sh; exsh smoke/ks3.sh {R}", "a\n"),
    ("sub_keeps_sh_c", f"sh -c 'x=$(echo a); echo \"$x\"' {R}", "a\n"),
    ("sub_keeps_background", f'( x=$(echo a); echo "$x" ) {R} & wait $!', "a\n"),
    ("sub_stdout_closed", f'( exec >&-; x=$(echo a); echo "[$x]" {R} )', "[a]\n"),
    # With stdout closed a write fails, and the text goes nowhere: it used to reach the session's
    # terminal with status 0. So the status is what shows it. Applets and programs have their own
    # say in their status, so those rows only show that the shell goes on.
    ("closed_echo", f'( exec >&-; echo a ) 2>/dev/null; echo "rc $?" {R}', "rc 1\n"),
    ("closed_printf", f"( exec >&-; printf 'a\\n' ) 2>/dev/null; echo \"rc $?\" {R}", "rc 1\n"),
    ("closed_pwd", f'( exec >&-; pwd ) 2>/dev/null; echo "rc $?" {R}', "rc 1\n"),
    ("closed_type", f'( exec >&-; type echo ) 2>/dev/null; echo "rc $?" {R}', "rc 1\n"),
    ("closed_dirs", f'( exec >&-; dirs ) 2>/dev/null; echo "rc $?" {R}', "rc 1\n"),
    ("closed_pushd", f'( exec >&-; pushd . ) 2>/dev/null; echo "rc $?" {R}', "rc 0\n"),
    ("closed_nested", f'( exec >&-; ( echo a ) 2>/dev/null; echo "rc $?" {R} )', "rc 1\n"),
    ("closed_no_text", f'( exec >&-; true; echo "rc $?" {R} )', "rc 0\n"),
    ("closed_command", f'echo a >&- 2>/dev/null; echo "rc $?" {R}', "rc 1\n"),
    ("closed_group", f'{{ echo a; }} >&- 2>/dev/null; echo "rc $?" {R}', "rc 1\n"),
    ("closed_at_top", f'exec 8>&1; exec >&-; echo a 2>/dev/null; s=$?; exec >&8 8>&-; echo "rc $s" {R}; unset s', "rc 1\n"),
    ("closed_copy", f'( exec >&-; echo a 2>&1; echo "rc $?" {R} ) 2>/dev/null', "rc 1\n"),
    ("closed_stage", f'( exec >&-; true | {{ echo a; }} 2>/dev/null; echo "rc $?" {R} )', "rc 1\n"),
    ("closed_background", f'( exec >&-; {{ echo a; }} 2>/dev/null & wait $!; echo "rc $?" {R} )', "rc 1\n"),
    ("closed_stderr_copy", f'( exec 2>&-; echo a >&2; echo "rc $?" {R} )', "rc 1\n"),
    ("closed_applet_goes_on", f'( exec >&-; cat smoke/d.txt; ls; echo on {R} ) 2>/dev/null', "on\n"),
    ("closed_frontend_goes_on", f'( exec >&-; clike smoke/hello.clike; pascal smoke/hello.pas; echo on {R} ) 2>/dev/null', "on\n"),
    ("closed_then_stdout_works", f'( exec >&-; echo lost 2>/dev/null; exec {R}; echo kept )', "kept\n"),
    ("sub_keeps_killed", f'( x=$(sleep 30); echo no ) {R} & p=$!; sleep 1; kill $p; wait $p; y=$(echo next); echo "$y" {A}; unset y', "next\n"),
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
    # return from a loop in a function left the loop's frame on the stack, and the caller's
    # loop advanced that one instead of its own. c bounds them, so a wrong build fails and does
    # not hang.
    ("return_in_loop", f"rl() {{ while :; do return 4; done; }}; c=0; for x in 1 2; do c=$((c+1)); [ $c -gt 9 ] && break; rl; echo \"x $x $?\"; done {R}; unset -f rl", "x 1 4\nx 2 4\n"),
    ("return_in_loop_while", f"rl() {{ for y in 1 2 3; do return; done; }}; n=0; while [ $n -lt 2 ]; do n=$((n+1)); [ $n -gt 9 ] && break; rl; echo \"n $n\"; done {R}; unset -f rl", "n 1\nn 2\n"),
    ("return_in_loop_stage", f"rl() {{ while :; do return; done; }}; c=0; for x in 1 2; do c=$((c+1)); [ $c -gt 9 ] && break; rl; echo \"x $x\"; done | cat {R}; unset -f rl", "x 1\nx 2\n"),
    ("return_in_loop_background", f"rl() {{ while :; do return; done; }}; c=0; {{ for x in 1 2; do c=$((c+1)); [ $c -gt 9 ] && break; rl; echo \"x $x\"; done; }} {R} & wait $!; unset -f rl", "x 1\nx 2\n"),
    ("return_in_loop_redirect", f"rl() {{ while :; do echo in; return; done > smoke/rl.out; }}; {{ rl; echo back; }} {R}; unset -f rl", "back\n"),
    # <> was parsed as <, so a write through it failed and it never made the file.
    ("read_write_makes", f"exec 3<> smoke/rw.txt; echo written >&3; exec 3>&-; cat smoke/rw.txt {R}", "written\n"),
    ("read_write_keeps", f"printf 'one\\ntwo\\n' > smoke/rk.txt; exec 3<> smoke/rk.txt; read a <&3; echo TWO >&3; exec 3>&-; {{ echo \"$a\"; cat smoke/rk.txt; }} {R}", "one\none\nTWO\n"),
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
    # From five levels down this wrote into freed memory, and an abort ends the whole app.
    ("substitution_depth",
     f"sum() {{ if [ \"$1\" -le 0 ]; then echo 0; else echo $(( $1 + $(sum $(( $1 - 1 ))) )); fi; }}; "
     f"{{ sum 5; sum 8; sum 12; }} {R}", "15\n36\n78\n"),
    # The stages it started used to go on with the subshell's state after it was freed.
    ("debug_trap_exit", f"{{ ( trap 'exit 4' DEBUG; true | cat | cat; echo no ); echo $?; }} {R}", "4\n"),
    ("at_once_background", f"{{ clike --no-cache smoke/v.clike & pascal --no-cache smoke/v.pas & rea --no-cache smoke/v.rea & aether --no-cache smoke/v.aether & wait; }} 2>&1 | sort {R}", "aether 3 b true\nclike 3 5 7\npascal 3 a TRUE\nrea 3 3 true\n"),
    # One printf, one write: clike's used to go out a byte at a time and interleave mid-line.
    ("at_once_same_lines", f"{{ clike --no-cache smoke/v.clike & clike --no-cache smoke/x.clike & clike --no-cache smoke/v.clike & clike --no-cache smoke/x.clike & wait; }} 2>&1 | sort {R}", "clike 3 5 7\nclike 3 5 7\nclike2 4 9 8\nclike2 4 9 8\n"),
    # A background frontend says nothing of its own. A subshell announces no jobs, and a prompt's
    # announcement goes to stderr, as under bash.
    ("bg_no_debug_lines", f"{{ pascal smoke/hello.pas & wait; }} 2>&1 | cat {R}", "pascal ok 42\n"),
    ("bg_notice_on_stderr", f"x=$(pascal smoke/hello.pas > /dev/null & wait); echo \"[$x]\" {R}", "[]\n"),
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
    # One file with nothing at the destination takes its name, as with GNU rsync. A slash, a directory or two files mean a directory.
    ("rsync_file", f"rsync smoke/d.txt smoke/rs/one {R}; [ -f smoke/rs/one ] && cat smoke/rs/one {A}", "b\na\nc\n"),
    ("rsync_file_name", f"rsync -a smoke/rs/a/f smoke/rs/nm {R}; echo \"rc=$?\" {A}; [ -f smoke/rs/nm ] && cat smoke/rs/nm {A}; echo y > smoke/rs/nm2; touch -t 202001020304 smoke/rs/nm2; rsync smoke/d.txt smoke/rs/nm2 {A}; cat smoke/rs/nm2 {A}", "rc=0\nx\nb\na\nc\n"),
    ("rsync_file_slash", f"rsync smoke/d.txt smoke/rs/sl1/ {R}; cat smoke/rs/sl1/d.txt {A}", "b\na\nc\n"),
    ("rsync_file_dir", f"mkdir smoke/rs/ed; rsync smoke/d.txt smoke/rs/ed {R}; cat smoke/rs/ed/d.txt {A}; ln -s ed smoke/rs/ld; rsync smoke/rs/a/f smoke/rs/ld {A}; ls smoke/rs/ed {A}; readlink smoke/rs/ld {A}", "b\na\nc\nd.txt\nf\ned\n"),
    ("rsync_two_files", f"rsync smoke/d.txt smoke/rs/a/f smoke/rs/two {R}; ls smoke/rs/two {A}", "d.txt\nf\n"),
    # A link goes, not what it points at.
    ("rsync_file_link", f"echo y > smoke/rs/lt; ln -s lt smoke/rs/lf; ln -s nowhere smoke/rs/lg; rsync smoke/rs/a/f smoke/rs/lf {R}; rsync smoke/rs/a/f smoke/rs/lg {A}; cat smoke/rs/lt smoke/rs/lf smoke/rs/lg {A}; [ -L smoke/rs/lf ] || [ -L smoke/rs/lg ] || echo gone {A}", "y\nx\nx\ngone\n"),
    # Nothing left behind, and a status that says why.
    ("rsync_file_no_parent", f"rsync smoke/d.txt smoke/rs/nope/x/new > /dev/null 2>&1; echo \"rc=$?\" {R}; ls smoke/rs/nope {A}", "rc=3\nls: cannot access 'smoke/rs/nope': No such file or directory\n"),
    ("rsync_two_onto_file", f"echo y > smoke/rs/tf; rsync smoke/d.txt smoke/rs/a/f smoke/rs/tf > /dev/null 2>&1; echo \"rc=$?\" {R}; cat smoke/rs/tf {A}", "rc=3\ny\n"),
    ("rsync_file_dry_run", f"rsync -n smoke/d.txt smoke/rs/fdr {R}; ls smoke/rs/fdr {A}", "ls: cannot access 'smoke/rs/fdr': No such file or directory\n"),
    ("rsync_file_cwd", f"( cd smoke/rs; rsync a/f inhere; cat inhere ) {R}", "x\n"),
    # A device isn't anyone's file to replace, and it still works after.
    ("rsync_dev_null", f"rsync smoke/d.txt /dev/null > /dev/null 2>&1; [ $? -ne 0 ] && echo failed {R}; echo x > /dev/null; echo \"rc=$?\" {A}; cat /dev/null {A}", "failed\nrc=0\n"),
    # The list holds the app's own paths, not the container's.
    ("rsync_progress", f"rsync --progress -a smoke/rs/a smoke/rs/pr 2>&1 | grep -c Documents {R}", "0\n"),
    ("rsync_error_path", f"rsync -a smoke/nope smoke/rs/ep 2>&1 | head -1 {R}", "rsync: error: /home/smoke/nope: lstat: No such file or directory\n"),
    ("rsync_abs", f"rsync -a /home/smoke/rs/a /home/smoke/rs/abs {R}; cat smoke/rs/abs/a/b/g {A}", "y\n"),
    ("rsync_in_subshell", f"( cd smoke/rs; rsync -a a sub ) {R}; cat smoke/rs/sub/a/f {A}", "x\n"),
    ("rsync_update", f"rsync -a smoke/rs/a smoke/rs/up {R}; cp smoke/d.txt smoke/rs/a/h; rsync -a smoke/rs/a smoke/rs/up {A}; cat smoke/rs/up/a/h {A}; rm smoke/rs/a/h", "b\na\nc\n"),
    # Big enough to fill the socket between the two ends, then again to send it as a delta.
    ("rsync_big", f"seq 1 200000 > smoke/rs/big; rsync smoke/rs/big smoke/rs/bd {R}; cmp smoke/rs/big smoke/rs/bd {A}; echo more >> smoke/rs/big; rsync smoke/rs/big smoke/rs/bd {A}; cmp smoke/rs/big smoke/rs/bd {A}", ""),
    ("rsync_symlink", f"ln -s f smoke/rs/a/l; rsync -a smoke/rs/a smoke/rs/sl {R}; rm smoke/rs/a/l; readlink smoke/rs/sl/a/l {A}", "f\n"),
    ("rsync_meta", f"chmod 640 smoke/rs/a/f; touch -t 202001020304 smoke/rs/a/f; rsync -a smoke/rs/a smoke/rs/me {R}; stat -c %a smoke/rs/me/a/f {A}; date -r smoke/rs/me/a/f +%Y {A}", "640\n2020\n"),
    ("rsync_delete", f"mkdir smoke/rs/del; echo z > smoke/rs/del/old; rsync -a --delete smoke/rs/a/ smoke/rs/del {R}; ls smoke/rs/del {A}", "b\nf\n"),
    # A kept file deleted by mistake would be copied back, so its number shows it where ls would not.
    ("rsync_delete_keeps", f"( mkdir -p smoke/rs/dk/s/sub smoke/rs/dk/d; echo x > smoke/rs/dk/s/keep; echo y > smoke/rs/dk/s/sub/keep2; rsync -a smoke/rs/dk/s/ smoke/rs/dk/d; set -- $(ls -i smoke/rs/dk/d/keep smoke/rs/dk/d/sub/keep2); a=\"$1 $3\"; echo o > smoke/rs/dk/d/old; mkdir smoke/rs/dk/d/olddir; echo o > smoke/rs/dk/d/olddir/f; echo o > smoke/rs/dk/d/sub/old2; rsync -a --delete smoke/rs/dk/s/ smoke/rs/dk/d; set -- $(ls -i smoke/rs/dk/d/keep smoke/rs/dk/d/sub/keep2); [ \"$a\" = \"$1 $3\" ] && echo same; cd smoke/rs/dk/d; find . | sort ) {R}", "same\n.\n./keep\n./sub\n./sub/keep2\n"),
    # Two at once: libc's hcreate has one table per process, so the second failed and destroyed the first's.
    ("rsync_delete_together", f"mkdir -p smoke/rs/dt; for s in 1 2; do mkdir -p smoke/rs/dt/s$s smoke/rs/dt/d$s; i=0; while [ $i -lt 100 ]; do echo $s > smoke/rs/dt/s$s/f$i; i=$((i+1)); done; rsync -a smoke/rs/dt/s$s/ smoke/rs/dt/d$s; done > smoke/rs/dt/log 2>&1; n=0; while [ $n -lt 10 ]; do echo o > smoke/rs/dt/d1/old; echo o > smoke/rs/dt/d2/old; rsync -a --delete smoke/rs/dt/s1/ smoke/rs/dt/d1 >> smoke/rs/dt/log 2>&1 & rsync -a --delete smoke/rs/dt/s2/ smoke/rs/dt/d2 >> smoke/rs/dt/log 2>&1 & wait; n=$((n+1)); done > smoke/rs/dt/msg 2>&1; cat smoke/rs/dt/log {R}; ls smoke/rs/dt/d1 | wc -l | tr -d ' ' {A}; ls smoke/rs/dt/d2 | wc -l | tr -d ' ' {A}", "100\n100\n"),
    ("rsync_exclude_once", f"rsync -a --exclude f smoke/rs/a/ smoke/rs/e1 > /dev/null 2>&1; rsync -a smoke/rs/a/ smoke/rs/e2 {R}; ls smoke/rs/e1 smoke/rs/e2 {A}", "smoke/rs/e1:\nb\n\nsmoke/rs/e2:\nb\nf\n"),
    ("rsync_dry_run", f"rsync -an smoke/rs/a smoke/rs/dry {R}; ls smoke/rs/dry {A}", "ls: cannot access 'smoke/rs/dry': No such file or directory\n"),
    ("rsync_pipe", f"rsync -a smoke/rs/a smoke/rs/pi | cat {R}; cat smoke/rs/pi/a/f {A}", "x\n"),
    ("rsync_background", f"rsync -a smoke/rs/a smoke/rs/bg > /dev/null 2>&1 & wait $!; echo \"wait $?\" {R}; cat smoke/rs/bg/a/f {A}", "wait 0\nx\n"),
    ("rsync_loop", f"for i in 1 2 3 4 5 6 7 8; do rsync -a smoke/rs/a smoke/rs/lp$i {A}; echo $? {A}; done", "0\n0\n0\n0\n0\n0\n0\n0\n"),
    ("rsync_two_at_once", f"rsync -a --exclude f smoke/rs/a/ smoke/rs/t1 > /dev/null 2>&1 & p=$!; rsync -a smoke/rs/a/ smoke/rs/t2 {R}; wait $p; ls smoke/rs/t1 smoke/rs/t2 {A}", "smoke/rs/t1:\nb\n\nsmoke/rs/t2:\nb\nf\n"),
    # The receiver zeroes the file mask, as a process about to exit may. That stays in rsync.
    ("rsync_keeps_mask", f"umask 027; rsync -a smoke/rs/a smoke/rs/km {R}; umask {A}; : > smoke/rs/kmf; umask 022; ls -l smoke/rs/kmf | cut -c1-10 {A}", "0027\n-rw-r-----\n"),
    ("rsync_keeps_job_mask", f"( umask 027; rsync -a smoke/rs/a smoke/rs/kj; umask; : > smoke/rs/kjf ) {R}; ls -l smoke/rs/kjf | cut -c1-10 {A}", "0027\n-rw-r-----\n"),
    # These fail with a status, and the app carries on.
    ("rsync_no_source", f"rsync -a smoke/nope smoke/rs/no > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_bad_dest", f"rsync -a smoke/rs/a smoke/d.txt/x > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=3\n"),
    ("rsync_no_server", f"rsync -a --rsync-path=nope smoke/rs/a smoke/rs/ns > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_no_shell", f"rsync -a -e nope smoke/rs/a host:x > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    ("rsync_usage", f"rsync > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    # A transport that fails. No ssh connects: the first program does not exist, and the
    # others stop at the name lookup. A SIGPIPE handler of scp's ended the app from the main
    # thread, and closing its one socket twice closed a host descriptor with that number.
    ("scp_no_program", f"scp -S nope smoke/d.txt h:x > /dev/null 2>&1; [ $? -ne 0 ] && echo failed {R}", "failed\n"),
    ("sftp_no_program", f"sftp -S nope h < /dev/null > /dev/null 2>&1; [ $? -ne 0 ] && echo failed {R}", "failed\n"),
    ("scp_no_host", f"scp smoke/d.txt nosuch.invalid:x > /dev/null 2>&1; [ $? -ne 0 ] && echo failed {R}", "failed\n"),
    ("sftp_no_host", f"sftp nosuch.invalid < /dev/null > /dev/null 2>&1; [ $? -ne 0 ] && echo failed {R}", "failed\n"),
    ("rsync_after_failures", f"rsync -a smoke/rs/a smoke/rs/af {R}; echo \"rc=$?\" {A}", "rc=0\n"),
    # An ssh command runs here, not in a tab of its own, so it keeps its redirections and its
    # status. Nothing listens on port 1, so ssh fails with 255, where the tab returned 0 and
    # left the file empty. Data reaching a remote command, and rsync to a remote host, need a
    # server, which this tool doesn't start: check those against a throwaway sshd by hand.
    ("ssh_redirect", f"ssh -p 1 -o BatchMode=yes 127.0.0.1 echo x > smoke/sshr 2>&1; echo \"rc=$?\" {R}; grep -c 'Connection refused' smoke/sshr {A}", "rc=255\n1\n"),
    ("ssh_pipe_out", f"ssh -p 1 -o BatchMode=yes 127.0.0.1 echo x 2>&1 | grep -c 'Connection refused' {R}", "1\n"),
    ("ssh_pipe_in", f"echo x | ssh -p 1 -o BatchMode=yes 127.0.0.1 cat > /dev/null 2>&1; echo \"rc=$?\" {R}", "rc=255\n"),
    ("ssh_config", f"ssh -G example.invalid 2>&1 | grep '^hostname ' {R}", "hostname example.invalid\n"),
    ("ssh_version", f"ssh -V 2>&1 | grep -c OpenSSH {R}", "1\n"),
    # rsync reaches ssh without going through the applet.
    ("rsync_ssh_refused", f"rsync -a -e 'ssh -p 1 -o BatchMode=yes' smoke/rs/a 127.0.0.1:x 2>&1 | grep -c 'Connection refused' {R}", "1\n"),
    # So do scp and sftp. A file on stderr is a host descriptor, and a sweep at ssh's start closed
    # those: the message went nowhere, and against a server ssh lost its stdin and stdout the same way.
    ("rsync_ssh_stderr_file", f"rsync -a -e 'ssh -p 1 -o BatchMode=yes' smoke/rs/a 127.0.0.1:x > smoke/rsse 2>&1; grep -c 'Connection refused' smoke/rsse {R}", "1\n"),
    ("scp_ssh_stderr_file", f"scp -P 1 -o BatchMode=yes smoke/d.txt 127.0.0.1:x > smoke/scse 2>&1; grep -c 'Connection refused' smoke/scse {R}", "1\n"),
    ("sftp_ssh_stderr_file", f"sftp -P 1 -o BatchMode=yes 127.0.0.1 < /dev/null > smoke/sfse 2>&1; grep -c 'Connection refused' smoke/sfse {R}", "1\n"),
    # The name's second letter made ui/f an -i option, so /f was expanded and scp couldn't find it.
    ("scp_name_like_option", f"cd smoke; mkdir -p ui; cp d.txt ui/f; scp -P 1 -o BatchMode=yes ui/f 127.0.0.1:x > scui 2>&1; cd \"$HOME\"; grep -c 'Connection refused' smoke/scui {R}", "1\n"),
    # An exit status of 1 is just a status. It came back as 130, and the shell dropped the rest of the line as it does after ^C.
    ("scp_status_one", f"scp -O -P 1 -o BatchMode=yes smoke/d.txt 127.0.0.1:x > smoke/sc1 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    # scp waits for its ssh in the task table and gets ssh's 255. The host's waitpid knew no such child, so a good copy ended with 1.
    ("scp_wait_status", f"scp -O -v -P 1 -o BatchMode=yes 127.0.0.1:x smoke/sc2 > smoke/sc2.log 2>&1; grep -c 'ended with wait status 0xff00' smoke/sc2.log {R}", "1\n"),
    # Each source connects again, after collecting the last one's ssh.
    ("scp_two_sources", f"scp -O -P 1 -o BatchMode=yes 127.0.0.1:a 127.0.0.1:b smoke/ > smoke/sc3.log 2>&1; echo \"rc=$?\" {R}", "rc=1\n"),
    # Two clients at once keep their own arguments. libc's getopt state and scp's and sftp's variables were the whole app's,
    # so one could take the other's host or lose its port. These depend on timing, so a wrong build fails them often, not
    # always. The hosts can't resolve, so a wrong build can't reach a real sshd: losing -P once meant port 22.
    ("ssh_two_at_once", f"i=0; bad=0; while [ $i -lt 10 ]; do i=$((i+1)); ssh -G a.invalid > smoke/ga 2>&1 & ssh -G b.invalid > smoke/gb 2>&1 & wait; grep -q '^hostname a.invalid' smoke/ga || bad=$((bad+1)); grep -q '^hostname b.invalid' smoke/gb || bad=$((bad+1)); done; echo \"bad=$bad\" {R}", "bad=0\n"),
    ("scp_two_at_once", f"i=0; bad=0; while [ $i -lt 10 ]; do i=$((i+1)); scp -O -o BatchMode=yes smoke/d.txt a.invalid:x > smoke/p1 2>&1 & scp -O -o BatchMode=yes smoke/d.txt b.invalid:x > smoke/p2 2>&1 & wait; grep -q 'hostname a.invalid' smoke/p1 || bad=$((bad+1)); grep -q 'hostname b.invalid' smoke/p2 || bad=$((bad+1)); done; echo \"bad=$bad\" {R}", "bad=0\n"),
    ("sftp_two_at_once", f"i=0; bad=0; while [ $i -lt 10 ]; do i=$((i+1)); sftp -o BatchMode=yes a.invalid < /dev/null > smoke/p1 2>&1 & sftp -o BatchMode=yes b.invalid < /dev/null > smoke/p2 2>&1 & wait; grep -q 'hostname a.invalid' smoke/p1 || bad=$((bad+1)); grep -q 'hostname b.invalid' smoke/p2 || bad=$((bad+1)); done; echo \"bad=$bad\" {R}", "bad=0\n"),
    # scp -3 -O starts its two ssh clients with nothing between them.
    ("scp_3_old_parses", f"i=0; while [ $i -lt 5 ]; do i=$((i+1)); scp -3 -O -o BatchMode=yes a.invalid:x b.invalid:y >> smoke/p3 2>&1; done; grep -c 'invalid characters' smoke/p3 {R}", "0\n"),
    # The child can't start a shell here, and its kill(getpid(), SIGTERM) was the host's: it ended the whole app.
    ("ssh_match_exec", f"printf 'Match exec \"true\"\\n  Port 23\\n' > smoke/mx.cfg; ssh -G -F smoke/mx.cfg 127.0.0.1 > smoke/mx.log 2>&1; echo alive {R}", "alive\n"),
    # A signal stops an ssh that waits on a server, which here never says a word. Each has 20 s
    # to give up by itself, with 255, so a signal that does nothing fails rather than hangs.
    ("ssh_stop_int", f"ssh -p {SILENT_PORT} -o BatchMode=yes -o ConnectTimeout=20 -o ConnectionAttempts=1 127.0.0.1 echo x > /dev/null 2>&1 & p=$!; sleep 2; kill -INT $p; wait $p; echo \"rc=$?\" {R}", "rc=130\n"),
    ("ssh_stop_term", f"ssh -p {SILENT_PORT} -o BatchMode=yes -o ConnectTimeout=20 -o ConnectionAttempts=1 127.0.0.1 echo x > /dev/null 2>&1 & p=$!; sleep 2; kill -TERM $p; wait $p; echo \"rc=$?\" {R}", "rc=143\n"),
    ("ssh_after_stops", f"ssh -p 1 -o BatchMode=yes 127.0.0.1 echo x > smoke/ssha 2>&1; echo \"rc=$?\" {R}; grep -c 'Connection refused' smoke/ssha {A}", "rc=255\n1\n"),
    # A program's file builtins follow cd and take the app's absolute paths. They used the
    # process's directory, with no error. Each directory has its own copy of each name, so
    # vmshow tells which copies were read, written, renamed and erased.
    ("vm_pascal", f'vmfiles; cd smoke/v1; pascal ../vmio.pas > {O} 2>&1; cd "$HOME"; vmshow {A}', "read in1\nsize 4\nexists TRUE FALSE FALSE\ncwd /home/smoke/v1\nrename 0\nerase 0\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.out: wrote in1\nappended\nsmoke/v1/vm.ren2: ren1\n" + V2 + VH),
    ("vm_pascal_other_dir", f'vmfiles; cd smoke/v2; pascal ../vmio.pas > {O} 2>&1; cd "$HOME"; cat smoke/v2/vm.out {A}', "read in2\nsize 4\nexists TRUE FALSE FALSE\ncwd /home/smoke/v2\nrename 0\nerase 0\nwrote in2\nappended\n"),
    ("vm_clike", f'vmfiles; cd smoke/v1; clike ../vmio.clike > {O} 2>&1; cd "$HOME"; vmshow {A}', "read in1\nexists 1 0 0\ncwd /home/smoke/v1\nrename 0\nerase 0\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.out: wrote in1\nappended\nsmoke/v1/vm.ren2: ren1\n" + V2 + VH),
    ("vm_rea", f'vmfiles; cd smoke/v1; rea ../vmio.rea > {O} 2>&1; cd "$HOME"; vmshow {A}', "read in1\nexists true false false\ncwd /home/smoke/v1\nrename 0\nerase 0\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.out: wrote in1\nappended\nsmoke/v1/vm.ren2: ren1\n" + V2 + VH),
    ("vm_aether", f'vmfiles; cd smoke/v1; aether ../vmio.aether > {O} 2>&1; cd "$HOME"; vmshow {A}', "read in1\nexists true false false\ncwd /home/smoke/v1\nrename true\ndelete true\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.out: wrote in1\nappended\nsmoke/v1/vm.ren2: ren1\n" + V2 + VH),
    ("vm_abs_pascal", f'vmfiles; cd smoke/v2; pascal ../vmabs.pas > {O} 2>&1; cd "$HOME"; vmshow {A}', "read in1\nexists TRUE\nrename 0\nerase 0\nnull 0\nup in1\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.out: wrote in1\nsmoke/v1/vm.ren2: ren1\n" + V2 + VH),
    ("vm_abs_aether", f'vmfiles; cd smoke/v2; aether ../vmabs.aether > {O} 2>&1; cd "$HOME"; vmshow {A}', "read in1\nexists true\nrename true\ndelete true\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.out: wrote in1\nsmoke/v1/vm.ren2: ren1\n" + V2 + VH),
    ("vm_clike_fopen_mstream", f'vmfiles; cd smoke/v1; clike ../vmm.clike > {O} 2>&1; cd "$HOME"; vmshow {A}', "mstream in1\ndir only1\nattr 16\nsmoke/v1/vm.del: del1\nsmoke/v1/vm.fa: fopen abs\nsmoke/v1/vm.fo: fopen\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.ma: in1\nsmoke/v1/vm.ms: in1\nsmoke/v1/vm.ren: ren1\n" + V2 + VH),
    ("vm_aether_more", f'vmfiles; cd smoke/v1; aether ../vmx.aether > {O} 2>&1; cd "$HOME"; ls -d smoke/v1/vmk smoke/v1/vmx.db {A}; vmshow {A}', "rmdir 0 0\nmkdir 0\nmstream in1\n\ntoon 10\ntry toon true 10\nsqlite 0\nmemory 0\nhttp 200 in1\nhttp abs 200 in1\nhttp file 200\nasync 200 in1\n\nsmoke/v1/vmk\nsmoke/v1/vmx.db\nsmoke/v1/vm.del: del1\nsmoke/v1/vm.hf: in1\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.ms: in1\nsmoke/v1/vm.ren: ren1\n" + V2 + VH),
    ("vm_thread", f'vmfiles; cd smoke/v1; rea ../vmthread.rea > {O} 2>&1; cd "$HOME"; vmshow {A}', "main done\nsmoke/v1/vm.del: del1\nsmoke/v1/vm.in: in1\nsmoke/v1/vm.out: child in1\nsmoke/v1/vm.ren: ren1\n" + V2 + VH),
    # Each way of starting a frontend gives it its own vproc, and each has to inherit the directory.
    ("vm_subshell", f'vmfiles; ( cd smoke/v1; pascal ../vmctx.pas > {O} 2>&1 ); vmctx {A}', "ctx in1\nin1\n"),
    ("vm_pipeline_first", f'vmfiles; cd smoke/v1; pascal ../vmctx.pas | cat > {O} 2>&1; cd "$HOME"; vmctx {A}', "ctx in1\nin1\n"),
    ("vm_pipeline_last", f'vmfiles; cd smoke/v1; echo x | pascal ../vmctx.pas > {O} 2>&1; cd "$HOME"; vmctx {A}', "ctx in1\nin1\n"),
    ("vm_background", f'vmfiles; cd smoke/v1; pascal ../vmctx.pas > {O} 2>/dev/null & wait $!; cd "$HOME"; vmctx {A}', "ctx in1\nin1\n"),
    ("vm_substitution", f'vmfiles; cd smoke/v1; x=$(pascal ../vmctx.pas); cd "$HOME"; echo "$x" {R}; vmctx {A}', "ctx in1\nin1\n"),
    ("vm_sh_c", f"vmfiles; sh -c 'cd smoke/v1; pascal ../vmctx.pas' {R}; vmctx {A}", "ctx in1\nin1\n"),
    ("vm_exsh_script", f"vmfiles; printf 'cd smoke/v1\\npascal ../vmctx.pas\\n' > smoke/vc1.sh; exsh smoke/vc1.sh {R}; vmctx {A}", "ctx in1\nin1\n"),
    # A frontend on its own runs on a worker thread, which a nested shell's directory has to reach.
    ("vm_sh_c_clike", f"vmfiles; sh -c 'cd smoke/v1; clike ../vmio.clike > vmr.txt 2>&1'; head -1 smoke/v1/vmr.txt {R}", "read in1\n"),
    ("vm_sh_c_rea", f"vmfiles; sh -c 'cd smoke/v1; rea ../vmio.rea > vmr.txt 2>&1'; head -1 smoke/v1/vmr.txt {R}", "read in1\n"),
    ("vm_sh_c_aether", f"vmfiles; sh -c 'cd smoke/v1; aether ../vmio.aether > vmr.txt 2>&1'; head -1 smoke/v1/vmr.txt {R}", "read in1\n"),
    ("vm_sh_c_twice", f"vmfiles; sh -c 'cd smoke; sh -c \"cd v1; pascal ../vmctx.pas\"; pwd' {R}; pwd {A}; vmctx {A}", "ctx in1\n/home/smoke\n/home\nin1\n"),
    ("vm_sh_c_subshell", f"vmfiles; sh -c 'cd smoke/v1; ( pascal ../vmctx.pas )' {R}; vmctx {A}", "ctx in1\nin1\n"),
    ("vm_sh_c_shebang", f"vmfiles; printf '#!/bin/exsh\\ncat vm.in\\n' > smoke/vc3.sh; chmod +x smoke/vc3.sh; sh -c 'cd smoke/v1; ../vc3.sh' {R}", "in1\n"),
    ("vm_script_shebang", f"vmfiles; printf '#!/bin/exsh\\ncd smoke/v1\\npascal ../vmctx.pas\\n' > smoke/vc4.sh; chmod +x smoke/vc4.sh; smoke/vc4.sh {R}; vmctx {A}", "ctx in1\nin1\n"),
    ("vm_sourced", f"vmfiles; printf 'cd smoke/v1\\npascal ../vmctx.pas\\ncd \"$HOME\"\\n' > smoke/vc2.sh; . smoke/vc2.sh {R}; vmctx {A}", "ctx in1\nin1\n"),
    ("vm_tiny_cd", f'cd smoke; tiny p.tiny p3.pbc > {O} 2>&1; cd "$HOME"; ls smoke/p3.pbc {A}', "smoke/p3.pbc\n"),
    # Paths that are not plain files in the app's tree keep working.
    ("vm_devices_home", f'vmfiles; cd smoke/v2; pascal ../vmdev.pas > {O} 2>&1; cd "$HOME"', "urandom 0\nzero 0\nnull 0\ntty 0\nhome in1\n"),
    ("urandom_applet", f"head -c 4 /dev/urandom | wc -c {R}", "4\n"),
    # Nothing execs in the app: exec runs the program as a call and then the shell is over.
    ("exec_applet", f"( exec printf 'hi\\n' ) {R}; echo \"rc=$?\" {A}", "hi\nrc=0\n"),
    ("exec_sh", f"( exec sh -c 'echo inner; exit 3' ) {R}; echo \"rc=$?\" {A}", "inner\nrc=3\n"),
    ("exec_builtin_name", f"( exec echo hi ) {R}", "hi\n"),
    ("exec_plain_script", f"printf 'echo plain\\n' > smoke/ep.sh; chmod +x smoke/ep.sh; ( exec ./smoke/ep.sh ) {R}", "plain\n"),
    ("exec_shebang_script", f"printf '#!/bin/exsh\\necho shebang \"$@\"\\n' > smoke/eq.sh; chmod +x smoke/eq.sh; ( exec ./smoke/eq.sh a b ) {R}", "shebang a b\n"),
    ("exec_frontend", f"( exec pascal smoke/hello.pas ) {R}", "pascal ok 42\n"),
    ("exec_prefix", f"( ep=1 exec printenv ep ) {R}", "1\n"),
    ("exec_not_exported", f"en=1; ( exec sh -c 'echo \"[$en]\"' ) {R}", "[]\n"),
    ("exec_redirect", f"( exec printf 'x\\n' > smoke/er.txt ); cat smoke/er.txt {R}", "x\n"),
    ("exec_stdin", f"( exec cat < smoke/d.txt ) {R}", "b\na\nc\n"),
    ("exec_stdin_outer", f"( exec cat ) < smoke/d.txt {R}", "b\na\nc\n"),
    ("exec_stdin_kept", f"( exec < smoke/d.txt; exec cat ) {R}", "b\na\nc\n"),
    ("exec_background", f"( exec printf 'bg\\n' ) {R} & wait; echo \"rc=$?\" {A}", "bg\nrc=0\n"),
    ("exec_missing", f"( exec nosuchprog ) 2> /dev/null; echo \"rc=$?\" {R}", "rc=127\n"),
    # An empty name is a program that cannot be found, and a prompt stays.
    ("exec_empty", f'( exec "" ) 2> /dev/null; echo "rc=$?" {R}', "rc=127\n"),
    ("exec_empty_substitution", f'x=$(exec "" 2> /dev/null; echo no); echo "[$x] $?" {R}; unset x', "[] 127\n"),
    ("exec_empty_prompt", f'exec "" 2> /dev/null; echo "rc=$?" {R}', "rc=127\n"),
    # {name}> f: the shell picks the number, and it stays until the script closes it.
    ("fdvar_write", f'exec {{fv}}> smoke/fv1; echo hi >&$fv; exec {{fv}}>&-; cat smoke/fv1 {R}; unset fv', "hi\n"),
    ("fdvar_read", f'exec {{fv}}< smoke/d.txt; read l <&$fv; exec {{fv}}<&-; echo "$l" {R}; unset fv', "b\n"),
    ("fdvar_high", f'exec {{fv}}> /dev/null; [ "$fv" -ge 10 ] && echo high {R}; exec {{fv}}>&-; unset fv', "high\n"),
    ("fdvar_builtin_keeps", f'echo x {{fv}}> smoke/fv2 > /dev/null; echo y >&$fv; exec {{fv}}>&-; cat smoke/fv2 {R}; unset fv', "y\n"),
    ("fdvar_group_keeps", f'{{ echo in; }} {{fv}}> smoke/fv3 > /dev/null; echo out >&$fv; exec {{fv}}>&-; cat smoke/fv3 {R}; unset fv', "out\n"),
    ("fdvar_applet_own", f'cat smoke/d.txt {{fv}}< smoke/d.txt > /dev/null; echo "[${{fv:-none}}]" {R}', "[none]\n"),
    ("fdvar_stage_own", f'exec {{fv}}> /dev/null | cat; echo "[${{fv:-none}}]" {R}', "[none]\n"),
    ("fdvar_job_own", f'true {{fv}}> /dev/null & wait; echo "[${{fv:-none}}]" {R}', "[none]\n"),
    ("fdvar_substitution_own", f'x=$(exec {{fv}}> /dev/null; echo "$fv"); ( : >&$x ) 2> /dev/null; r=$?; echo "[${{fv:-none}}] $r" {R}; unset x r', "[none] 1\n"),
    ("fdvar_closed", f'exec {{fv}}>&2; exec {{fv}}>&-; ( echo no >&$fv ) 2> /dev/null; echo "rc $?" {R}; unset fv', "rc 1\n"),
    ("closed_stdout_stays", f'( exec >&-; {{ :; }} > /dev/null; echo leak ) 2> /dev/null; echo "rc=$?" {R}', "rc=1\n"),
    ("fd10_closed", f'exec 10>&2; exec 10>&-; ( echo no >&10 ) 2> /dev/null; echo "rc $?" {R}', "rc 1\n"),
    ("wait_n_copies", f'( sleep 0.2 & x=$(wait -n; echo "rc $?"; wait -n; echo "rc $?"); echo "[$x]" {R} )', "[rc 0\nrc 127]\n"),
    ("bang_operator", f'sleep 0 & wait; [ "${{!:-x}}" = "$!" ] && echo same {R}', "same\n"),
    ("bang_bad", f'( echo "${{!!}}" ) 2> /dev/null; echo "rc=$?" {R}', "rc=1\n"),
    ("procsub_fd3", f'exec 3< <(echo x); read l <&3; exec 3<&-; echo "[$l]" {R}', "[x]\n"),
    ("exec_function", f"exf() {{ echo fn; }}; ( exec exf ) 2> /dev/null; echo \"rc=$?\" {R}", "rc=127\n"),
    ("exec_shell_builtin", f"( exec cd / ) 2> /dev/null; echo \"rc=$?\" {R}", "rc=127\n"),
    ("exec_subst", f"x=$(exec printf in); echo \"[$x] $?\" {R}", "[in] 0\n"),
    ("exec_stage", f"exec printf 'piped\\n' | cat {R}", "piped\n"),
    ("exec_no_exit_trap", f"( trap 'echo bye' EXIT; exec printf 'hi\\n' ) {R}", "hi\n"),
    ("exec_c_empty_env", f"export ee=1; ( exec -c sh -c 'echo \"[$ee]\"' ) {R}; unset ee", "[]\n"),
    ("exec_shell_goes_on", f"( exec true ); echo after {R}", "after\n"),
    ("exec_script_ends", f"printf 'exec printf \"hi\\\\n\"\\necho never\\n' > smoke/es.sh; sh smoke/es.sh {R}; echo \"rc=$?\" {A}", "hi\nrc=0\n"),
    # And env does the same for what is not an applet.
    ("env_sh", f"env sh -c 'echo inner' {R}", "inner\n"),
    ("env_sh_var", f"env ev2=1 sh -c 'echo \"[$ev2]\"; exit 4' {R}; echo \"rc=$?\" {A}", "[1]\nrc=4\n"),
    ("env_sh_goes_no_further", f"echo \"[$ev2]\" {R}", "[]\n"),
    ("env_i_sh", f"export ek=1; env -i sh -c 'echo \"[$ek]\"' {R}; unset ek", "[]\n"),
    ("env_u_sh", f"export ek=1; env -u ek sh -c 'echo \"[$ek]\"' {R}; unset ek", "[]\n"),
    ("env_nested", f"env -- env sh -c 'echo nested' {R}", "nested\n"),
    ("env_stage", f"env sh -c 'echo first' | cat {R}", "first\n"),
    ("env_stage_stdin", f"printf 'x\\n' | env sh -c cat {R}", "x\n"),
    ("env_plain_script", f"printf 'echo \"plain [$ev3]\"\\n' > smoke/ev.sh; chmod +x smoke/ev.sh; env ev3=1 ./smoke/ev.sh {R}", "plain [1]\n"),
    ("env_shebang_script", f"printf '#!/bin/exsh\\necho \"shebang [$ev3]\"\\n' > smoke/ev2.sh; chmod +x smoke/ev2.sh; env ev3=1 ./smoke/ev2.sh {R}", "shebang [1]\n"),
    ("env_frontend", f"env pascal smoke/hello.pas {R}", "pascal ok 42\n"),
    ("env_frontend_var", f"env ep=9 clike smoke/envp.clike {R}", "[9]\n"),
    ("env_chdir_sh", f"env -C smoke/sub sh -c 'cat n.txt' {R}; pwd {A}", "needle\n/home\n"),
    ("env_missing", f"env nosuchprog 2> /dev/null; echo \"rc=$?\" {R}", "rc=127\n"),
    ("env_function", f"evf() {{ echo fn; }}; env evf 2> /dev/null; echo \"rc=$?\" {R}", "rc=127\n"),
    # nohup as well, and what it runs ignores a hangup. It reached a raw exec for everything.
    ("nohup_applet", f"nohup printf 'hi\\n' {R}; echo \"rc=$?\" {A}", "hi\nrc=0\n"),
    ("nohup_script", f"nohup smoke/sb.sh a b {R}; echo still {A}", "sb 2\nstill\n"),
    ("nohup_script_rc", f"nohup smoke/sb3.sh > /dev/null 2>&1; echo $? {R}", "3\n"),
    ("nohup_plain_script", f"printf 'echo \"plain [$nv]\"\\nexit 4\\n' > smoke/nh.sh; chmod +x smoke/nh.sh; nv=1 nohup ./smoke/nh.sh {R}; echo \"rc=$?\" {A}", "plain [1]\nrc=4\n"),
    ("nohup_frontend", f"nohup pascal smoke/hello.pas {R}", "pascal ok 42\n"),
    ("nohup_frontend_file", f"nohup Examples/clike/base/hello {R}", "Hello World!\n"),
    ("nohup_sh", f"nohup sh -c 'echo inner; exit 5' {R}; echo \"rc=$?\" {A}", "inner\nrc=5\n"),
    ("nohup_pipe", f"nohup smoke/sb.sh a | cat {R}", "sb 1\n"),
    ("nohup_stdin", f"printf 'x\\n' | nohup cat {R}", "x\n"),
    ("nohup_sub", f"x=$(nohup smoke/sb.sh a); echo \"got $x\" {R}", "got sb 1\n"),
    ("nohup_job", f"nohup smoke/sb.sh a {R} & wait $!; echo rc=$? {A}", "sb 1\nrc=0\n"),
    ("nohup_missing", f"nohup nosuchprog 2> /dev/null; echo \"rc=$?\" {R}", "rc=127\n"),
    ("nohup_function", f"nhf() {{ echo fn; }}; nohup nhf 2> /dev/null; echo \"rc=$?\" {R}", "rc=127\n"),
    ("nohup_not_executable", f"printf 'x\\n' > smoke/nx; chmod -x smoke/nx; nohup smoke/nx 2> /dev/null; echo \"rc=$?\" {R}", "rc=126\n"),
    ("nohup_ignores_hup", f"nohup sh -c 'kill -HUP $$; echo survived' {R}", "survived\n"),
    ("nohup_job_hup", f"nohup sleep 2 {R} & p=$!; sleep 1; kill -HUP $p; wait $p; echo rc=$? {A}", "rc=0\n"),
    ("nohup_shell_keeps_hup", f"{{ trap 'echo got' HUP; nohup true; kill -HUP $$; echo after; trap - HUP; }} {R}", "got\nafter\n"),
    ("nohup_terminal", f"rm -f nohup.out; nohup printf 'to-file\\n' 2> /dev/null; echo stays {R}; cat nohup.out {A}; rm -f nohup.out", "stays\nto-file\n"),
    # The app is not root, so chroot stops before it could run anything.
    ("chroot_refused", f"chroot smoke id -u 2> /dev/null; echo \"rc=$?\" {R}", "rc=1\n"),
    ("chroot_no_command", f"chroot smoke 2> /dev/null; echo \"rc=$?\" {R}", "rc=1\n"),
    # A shell does not believe a $PWD that does not name where it is.
    ("pwd_stale", f"PWD=/ sh -c 'echo \"$PWD\"' {R}", "/home\n"),
    ("pwd_stale_missing", f"PWD=/nonexistent sh -c 'echo \"$PWD\"; cd smoke/sub; pwd' {R}", "/home\n/home/smoke/sub\n"),
    ("pwd_stale_cat", f"( cd smoke/sub; PWD=/ sh -c 'cat n.txt; echo \"$PWD\"' ) {R}", "needle\n/home/smoke/sub\n"),
    ("pwd_right", f"( cd smoke/sub; sh -c 'echo \"$PWD\"' ) {R}", "/home/smoke/sub\n"),
    # Traps take a signal sent to the shell's own vproc, whoever sends it.
    # Killing a foreground pipeline's stages leaves the tab's shell: the last one is on its thread.
    ("kill_last_stage", f"( sleep 0.5; lps | grep '[s]leep 3.1' | while read kp kr; do kill -TERM $kp; done ) & sleep 3.1 | sleep 3.1; echo alive {A}", "alive\n"),
    # A foreground subshell or command is not the shell, so another task's signal ends it there.
    ("kill_foreground_subshell", f"( p=$BASHPID; ( sleep 0.3; kill -TERM $p ) & sleep 3; echo no {A} ); echo st=$? {A}", "st=143\n"),
    ("kill_foreground_command", f"( sleep 0.5; lps | grep '[s]eq 1 90000000' | while read kp kr; do kill -TERM $kp; done ) & seq 1 90000000 > /dev/null; echo st=$? {A}", "st=143\n"),
    ("trap_self", f"{{ trap 'echo got' USR1; kill -USR1 $$; echo after; trap - USR1; }} {R}", "got\nafter\n"),
    ("trap_function_local", f"{{ tf() {{ local tv=1; trap 'echo \"[$tv]\"' USR1; kill -USR1 $$; }}; tf; trap - USR1; }} {R}", "[1]\n"),
    ("trap_set_in_function", f"{{ tf() {{ trap 'echo later' USR1; }}; tf; kill -USR1 $$; echo after; trap - USR1; }} {R}", "later\nafter\n"),
    ("trap_child_sends", f"{{ trap 'echo got' USR1; sh -c 'kill -USR1 $PPID; echo child'; echo after; trap - USR1; }} {R}", "child\ngot\nafter\n"),
    ("trap_subshell_sends", f"{{ trap 'echo got' USR1; ( kill -USR1 $$ ); echo after; trap - USR1; }} {R}", "got\nafter\n"),
    ("trap_in_subshell", f"( trap 'echo got' USR1; sh -c 'kill -USR1 $PPID'; echo after ) {R}", "got\nafter\n"),
    ("trap_term", f"{{ trap 'echo term' TERM; kill -TERM $$; echo after; trap - TERM; }} {R}", "term\nafter\n"),
    ("trap_int", f"{{ trap 'echo int' INT; kill -INT $$; echo after; trap - INT; }} {R}", "int\nafter\n"),
    ("trap_status_kept", f"{{ trap 'false' USR1; kill -USR1 $$; echo \"rc=$?\"; trap - USR1; }} {R}", "rc=0\n"),
    ("trap_return", f"{{ tf() {{ trap 'return 5' USR1; kill -USR1 $$; echo never; }}; tf; echo \"rc=$?\"; trap - USR1; }} {R}", "rc=5\n"),
    ("trap_exit_subshell", f"( trap 'exit 4' USR1; sh -c 'kill -USR1 $PPID'; echo never ); echo \"rc=$?\" {R}", "rc=4\n"),
    ("trap_ignored", f"{{ trap '' USR1; kill -USR1 $$; echo after; trap - USR1; }} {R}", "after\n"),
    ("trap_stage_sends", f"{{ trap 'echo got' USR1; {{ kill -USR1 $$; echo piped; }} | cat; echo after; trap - USR1; }} {R}", "piped\ngot\nafter\n"),
    ("trap_builtin_stage_sends", f"{{ trap 'echo got' USR1; kill -USR1 $$ | cat; echo after; trap - USR1; }} {R}", "got\nafter\n"),
    ("trap_not_for_nested_shell", f"{{ trap 'echo got' USR1; sh -c 'trap; echo in'; trap - USR1; }} {R}", "in\n"),
    ("trap_nested_own", f"{{ trap 'echo outer' USR1; sh -c 'trap \"echo inner\" USR1; kill -USR1 $$; echo in'; echo out; trap - USR1; }} {R}", "inner\nin\nout\n"),
    # Stdout only: this list runs from a sourced file, where the app announces `[1] 123` on stderr.
    ("trap_not_for_child", f"{{ trap 'echo x' TERM; sleep 5 & tp=$!; kill -TERM $tp; wait $tp; echo \"rc=$?\"; trap - TERM; }} > {{o}}", "rc=143\n"),
    ("trap_chld_no_loop", f"{{ trap 'echo chld' CHLD; echo a; trap - CHLD; echo after; }} {R}", "a\nafter\n"),
    # The rest of that round: the same code as on a host, here on the app's threads.
    ("prefix_chain", f"pA=1 pB=$pA sh -c 'echo \"[$pA][$pB]\"' {R}; echo \"[$pA][$pB]\" {A}", "[1][1]\n[][]\n"),
    ("prefix_chain_stage", f"pA=1 pB=$pA env | grep -E '^p[AB]=' | sort {R}", "pA=1\npB=1\n"),
    ("prefix_readonly", f"readonly pr=1; pr=2 sh -c 'echo \"ran [$pr]\"' 2> /dev/null {R}", "ran []\n"),
    ("unset_local_hides", f"uv=g; uf() {{ local uv=1; y=$(unset uv; echo \"[$uv]\"); echo \"$y\"; unset uv; echo \"[${{uv-unset}}]\"; }}; uf {R}; echo \"[$uv]\" {A}", "[]\n[unset]\n[g]\n"),
    ("read_trailing", f"read rq <<< 'a b '; echo \"[$rq]\" {R}", "[a b]\n"),
    ("read_trailing_colon", f"IFS=: read rp rq <<< '1:2:'; echo \"[$rp][$rq]\" {R}", "[1][2]\n"),
    ("read_reply", f"read <<< ' a b '; echo \"[$REPLY]\" {R}", "[ a b ]\n"),
    ("set_lists", f"sv='a b'; set | grep '^sv=' {R}", "sv='a b'\n"),
    ("set_lists_local", f"sf() {{ local sl=1; set | grep '^sl='; }}; sf {R}", "sl=1\n"),
    ("declare_lists", f"dv='a b'; declare | grep '^dv=' {R}", "dv='a b'\n"),
    ("declare_p_pending", f"export dpe; declare -p | grep -E ' dpe$' {R}; unset dpe", "declare -x dpe\n"),
    ("declare_p_local", f"df() {{ local dl=1; declare -p | grep -E ' dl='; }}; df {R}", "declare -- dl=\"1\"\n"),
    ("declare_i_stage", f"declare -i di=1; declare -i | grep -c '^declare -i di=' {R}", "1\n"),
    ("subshell_keeps_attribute", f"declare -i di2=1; ( di2=di2+1; echo \"$di2\" ) {R}; echo \"$di2\" {A}", "2\n1\n"),
    ("set_lists_array", f"sa=(1 'x y'); set | grep '^sa=' {R}", "sa=([0]=\"1\" [1]=\"x y\")\n"),
    # Last, so that the listing tests above never see these names in $HOME.
    ("vm_cleanup", f"rm -rf vm.* vmj.json smoke/v1 smoke/v2; ls vm.in 2>/dev/null | wc -l {R}", "0\n"),
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
    "utmp.rec": utmp_record("alice", "pts/1") + utmp_record("bob", "pts/2") + utmp_record("carol", "pts/0"),
    "hello.pas": "program Hello;\nbegin\n  writeln('pascal ok ', 6*7);\nend.\n",
    "hello.clike": 'int main() {\n    printf("clike ok %d\\n", 6*7);\n    return 0;\n}\n',
    "envp.clike": 'int main() {\n    printf("[%s]\\n", getenv("ep"));\n    return 0;\n}\n',
    "envp.pas": "program EnvP;\nbegin\n  writeln('[', getenv('ep'), ']');\nend.\n",
    "hello.rea": 'class Main {}\n\nwriteln("rea ok ", 6*7);\n',
    "hello.aether": '@pure\nfn answer(a: Int, b: Int) -> Int {\n    ret a * b;\n}\n\nfn main() -> Void {\n    fx { println("aether ok ", answer(6, 7)); }\n    ret;\n}\n',
    "spin.pas": "program Spin;\nvar i: integer;\nbegin\n  i := 0;\n  while true do i := 1 - i;\nend.\n",
    "slow.pas": "program Slow;\nuses crt;\nbegin\n  delay(800);\n  writeln('slow done');\nend.\n",
    "halt.pas": "program Halting;\nbegin\n  halt(3);\nend.\n",
    "bigout.pas": "program BigOut;\nvar i: integer;\nbegin\n  for i := 1 to 30000 do writeln(i);\nend.\n",
    "bigout.clike": 'int main() {\n    int i;\n    for (i = 1; i <= 30000; i = i + 1) {\n        printf("%d\\n", i);\n    }\n    return 0;\n}\n',
    "big.pas": "program Big;\nvar i: integer;\nbegin\n  for i := 1 to 100000 do writeln('line of big output');\nend.\n",
    "first.pas": "program First;\nuses crt;\nbegin\n  writeln('first stage');\n  delay(2000);\nend.\n",
    "halt2.pas": "program Halting2;\nbegin\n  writeln('before halt');\n  halt(3);\nend.\n",
    "sdlclose.pas": "program SdlClose;\nbegin\n  InitGraph(320, 240, 'smoke');\n  CloseGraph;\n  writeln('closed');\nend.\n",
    "sdlerror.pas": "program SdlError;\nbegin\n  InitGraph(320, 240, 'smoke');\n  GLBegin('no_such_primitive');\nend.\n",
    "glprims.pas": "program GlPrims;\nbegin\n  InitGraph(320, 240, 'smoke');\n  GLBegin('quad_strip');\n  GLEnd;\n  GLBegin('polygon');\n  GLEnd;\n  CloseGraph;\n  writeln('drawn');\nend.\n",
    "thread.rea": 'void child() {\n    writeln("child ok");\n}\n\nint t = spawn child();\njoin t;\nwriteln("main ok");\n',
    **AT_ONCE_FIXTURES,
    "esc.bin": "\x1b",
    "key.pas": "program Key;\nuses crt;\nvar c: char;\nbegin\n  c := readkey;\n  writeln(ord(c));\nend.\n",
    "key2.pas": "program Key2;\nuses crt;\nvar c: char;\nbegin\n  c := readkey;\n  writeln(ord(c));\n  c := readkey;\n  writeln(ord(c));\nend.\n",
    "where.pas": "program Where;\nuses crt;\nbegin\n  writeln(wherex > 0, ' ', wherey > 0);\nend.\n",
    "s1.pas": "program S1;\nvar s, s2, n, i: integer;\n    all: array[0..255] of integer;\nbegin\n  n := 0;\n  s := socketcreate(1);\n  while (s >= 0) and (s < 200) and (n < 255) do\n  begin\n    all[n] := s;\n    n := n + 1;\n    s := socketcreate(1);\n  end;\n  writeln(s);\n  writeln(socketpoll(s, 50, 1));\n  socketclose(s);\n  writeln(socketlasterror);\n  s2 := socketcreate(1);\n  writeln(s2);\n  socketclose(s2);\n  for i := 0 to n - 1 do socketclose(all[i]);\nend.\n",
    "sno.pas": "program Sno;\nvar s: integer;\nbegin\n  s := socketcreate(1);\n  writeln(s);\n  socketclose(s);\nend.\n",
    "tcp.pas": "program Tcp;\nvar l, c, a, port, i, n, s: integer;\n    all: array[0..255] of integer;\n    fill: boolean;\n    r: mstream;\nbegin\n  n := 0;\n  fill := getenv('FILL') <> '';\n  if fill then\n  begin\n    s := socketcreate(0);\n    while (s >= 0) and (s < 199) and (n < 255) do\n    begin\n      all[n] := s;\n      n := n + 1;\n      s := socketcreate(0);\n    end;\n    all[n] := s;\n    n := n + 1;\n  end;\n  port := 0;\n  i := 0;\n  while (port = 0) and (i < 200) do\n  begin\n    l := socketcreate(0);\n    if socketbindaddr(l, '127.0.0.1', 39100 + i) = 0 then port := 39100 + i else socketclose(l);\n    i := i + 1;\n  end;\n  writeln('listen ', socketlisten(l, 4));\n  writeln('no client ', socketpoll(l, 50, 1));\n  c := socketcreate(0);\n  writeln('connect ', socketconnect(c, '127.0.0.1', port));\n  writeln('client waiting ', socketpoll(l, 2000, 1));\n  a := socketaccept(l);\n  writeln('nothing sent ', socketpoll(a, 50, 1));\n  writeln('can write ', socketpoll(c, 2000, 2));\n  writeln('send ', socketsend(c, 'hello'));\n  writeln('data waiting ', socketpoll(a, 2000, 1));\n  r := socketreceive(a, 16);\n  writeln('received ', mstreambuffer(r));\n  mstreamfree(r);\n  writeln('drained ', socketpoll(a, 50, 1));\n  socketclose(c);\n  writeln('close ', socketlasterror);\n  writeln('peer gone ', socketpoll(a, 2000, 1));\n  socketclose(a);\n  socketclose(l);\n  if fill then writeln('numbers ', l);\n  for i := 0 to n - 1 do socketclose(all[i]);\nend.\n",
    "sock.pas": "program Sock;\nvar s: integer;\nbegin\n  s := socketcreate(0);\n  socketsetblocking(s, false);\n  writeln(socketlasterror);\n  socketclose(s);\n  writeln(socketlasterror);\nend.\n",
    "vmio.pas": """program VmIo;
var f: text; s: string;
begin
  assign(f, 'vm.in'); reset(f); readln(f, s); close(f);
  writeln('read ', s);
  writeln('size ', filesize(f));
  assign(f, 'vm.out'); rewrite(f); writeln(f, 'wrote ', s); close(f);
  assign(f, 'vm.out'); append(f); writeln(f, 'appended'); close(f);
  writeln('exists ', fileexists('vm.in'), ' ', fileexists('nope'), ' ', fileexists(''));
  writeln('cwd ', getcurrentdir());
  assign(f, 'vm.ren'); rename(f, 'vm.ren2'); writeln('rename ', ioresult);
  assign(f, 'vm.del'); erase(f); writeln('erase ', ioresult);
end.
""",
    "vmio.clike": """int main() {
    text f;
    str s;
    assign(f, "vm.in"); reset(f); readln(f, s); close(f);
    printf("read %s\\n", s);
    assign(f, "vm.out"); rewrite(f); fprintf(f, "wrote %s\\n", s); close(f);
    assign(f, "vm.out"); append(f); fprintf(f, "appended\\n"); close(f);
    printf("exists %d %d %d\\n", fileexists("vm.in"), fileexists("nope"), fileexists(""));
    printf("cwd %s\\n", getcurrentdir());
    assign(f, "vm.ren"); rename(f, "vm.ren2"); printf("rename %d\\n", ioresult());
    assign(f, "vm.del"); remove(f); printf("erase %d\\n", ioresult());
    return 0;
}
""",
    "vmio.rea": """class Main {}
text f;
str s;
assign(f, "vm.in"); reset(f); readln(f, s); close(f);
writeln("read ", s);
assign(f, "vm.out"); rewrite(f); writeln(f, "wrote ", s); close(f);
assign(f, "vm.out"); append(f); writeln(f, "appended"); close(f);
writeln("exists ", fileexists("vm.in"), " ", fileexists("nope"), " ", fileexists(""));
writeln("cwd ", getcurrentdir());
assign(f, "vm.ren"); rename(f, "vm.ren2"); writeln("rename ", ioresult());
assign(f, "vm.del"); erase(f); writeln("erase ", ioresult());
""",
    "vmio.aether": """fn main() -> Void {
    fx {
        let lines: Text[] = read_lines("vm.in");
        println("read ", lines[0]);
        write_file("vm.out", "wrote " + lines[0] + "\\n");
        append_file("vm.out", "appended\\n");
        println("exists ", file_exists("vm.in"), " ", file_exists("nope"), " ", file_exists(""));
        println("cwd ", get_current_dir());
        println("rename ", rename_file("vm.ren", "vm.ren2"));
        println("delete ", delete_file("vm.del"));
    }
    ret;
}
""",
    "vmabs.pas": """program VmAbs;
var f: text; s: string;
begin
  assign(f, '/home/smoke/v1/vm.in'); reset(f); readln(f, s); close(f);
  writeln('read ', s);
  assign(f, '/home/smoke/v1/vm.out'); rewrite(f); writeln(f, 'wrote ', s); close(f);
  writeln('exists ', fileexists('/home/smoke/v1/vm.in'));
  assign(f, '../v1/vm.ren'); rename(f, '/home/smoke/v1/vm.ren2'); writeln('rename ', ioresult);
  assign(f, '/home/smoke/v1/vm.del'); erase(f); writeln('erase ', ioresult);
  assign(f, '/dev/null'); rewrite(f); writeln('null ', ioresult); writeln(f, 'x'); close(f);
  assign(f, '../v1/vm.in'); reset(f); readln(f, s); close(f);
  writeln('up ', s);
end.
""",
    "vmabs.aether": """fn main() -> Void {
    fx {
        let lines: Text[] = read_lines("/home/smoke/v1/vm.in");
        println("read ", lines[0]);
        write_file("/home/smoke/v1/vm.out", "wrote " + lines[0] + "\\n");
        println("exists ", file_exists("/home/smoke/v1/vm.in"));
        println("rename ", rename_file("/home/smoke/v1/vm.ren", "/home/smoke/v1/vm.ren2"));
        println("delete ", delete_file("/home/smoke/v1/vm.del"));
    }
    ret;
}
""",
    "vmm.clike": """int main() {
    text f;
    str mode;
    mstream ms;
    mode = "wb";
    f = fopen("vm.fo", mode);
    fprintf(f, "fopen\\n");
    fclose(f);
    f = fopen("/home/smoke/v1/vm.fa", mode);
    fprintf(f, "fopen abs\\n");
    fclose(f);
    ms = mstreamcreate();
    mstreamloadfromfile(&ms, "vm.in");
    mstreamsavetofile(&ms, "vm.ms");
    mstreamsavetofile(&ms, "/home/smoke/v1/vm.ma");
    printf("mstream %s", mstreambuffer(ms));
    mstreamfree(&ms);
    printf("dir %s\\n", dosfindfirst("vmo"));
    printf("attr %d\\n", dosgetfattr("vmo"));
    return 0;
}
""",
    "vmx.aether": """fn main() -> Void {
    fx {
        println("rmdir ", mkdir("vmr"), " ", rmdir("vmr"));
        println("mkdir ", mkdir("vmk"));
        let ms: MStream = mstream_load("vm.in");
        println("mstream ", mstream_text(ms));
        mstream_save(ms, "vm.ms");
        mstream_free(ms);
        let doc: ToonDoc = toon_parse_file("vmj.json");
        println("toon ", toon_get_int(toon_root(doc), "n"));
        toon_close(doc);
        let (tried, ok) = try_toon_parse_file("vmj.json");
        println("try toon ", ok, " ", toon_get_int(toon_root(tried), "n"));
        toon_close(tried);
        let db: Int = sqlite_open("vmx.db");
        println("sqlite ", sqlite_exec(db, "CREATE TABLE t(a INTEGER);"));
        sqlite_close(db);
        let mem: Int = sqlite_open(":memory:");
        println("memory ", sqlite_exec(mem, "CREATE TABLE t(a INTEGER);"));
        sqlite_close(mem);
        let s: Int = http_session();
        let out: MStream = mstream_new();
        println("http ", http_request(s, "GET", "file://vm.in", "", out), " ", mstream_text(out));
        println("http abs ", http_request(s, "GET", "file:///home/smoke/v1/vm.in", "", out), " ", mstream_text(out));
        println("http file ", http_request_to_file(s, "GET", "file://vm.in", "", "vm.hf"));
        let id: Int = http_request_async(s, "GET", "file://vm.in", "");
        loop !http_is_done(id) {
            sleep(1);
        }
        println("async ", http_await(id, out), " ", mstream_text(out));
        http_close(s);
    }
    ret;
}
""",
    "vmthread.rea": """void child() {
    text f;
    str s;
    assign(f, "vm.in"); reset(f); readln(f, s); close(f);
    assign(f, "vm.out"); rewrite(f); writeln(f, "child ", s); close(f);
}
int t = spawn child();
join t;
writeln("main done");
""",
    "vmdev.pas": """program VmDev;
var f: text; s: string;
begin
  assign(f, '/dev/urandom'); reset(f); writeln('urandom ', ioresult); close(f);
  assign(f, '/dev/zero'); reset(f); writeln('zero ', ioresult); close(f);
  assign(f, '/dev/null'); rewrite(f); writeln('null ', ioresult); writeln(f, 'x'); close(f);
  assign(f, '/dev/tty'); rewrite(f); writeln('tty ', ioresult); writeln(f, ''); close(f);
  assign(f, getenv('HOME') + '/smoke/v1/vm.in'); reset(f); readln(f, s); close(f);
  writeln('home ', s);
end.
""",
    "vmctx.pas": """program VmCtx;
var f: text; s: string;
begin
  assign(f, 'vm.in'); reset(f); readln(f, s); close(f);
  assign(f, 'vm.out'); rewrite(f); writeln(f, s); close(f);
  writeln('ctx ', s);
end.
""",
    "ctl.txt": "^cat\n^Zoo\na\x03b\n",
    "rl1.pas": "program Rl1;\nvar s: string;\nbegin\n  readln(s);\n  writeln('got ', s);\nend.\n",
    "rl3.pas": "program Rl3;\nvar a, b, c: string;\nbegin\n  readln(a);\n  readln(b);\n  readln(c);\n"
               "  writeln('[', a, '][', b, '][', c, ']');\nend.\n",
    "rlcount.pas": "program RlCount;\nvar s, last: string; n: integer;\nbegin\n  n := 0;\n  readln(s);\n"
                   "  while (s <> 'end') and (n < 60000) do\n  begin\n    n := n + 1;\n    last := s;\n    readln(s);\n  end;\n"
                   "  writeln(n, ' ', last);\nend.\n",
    "rdmix.pas": "program RdMix;\nvar a, b: integer; s, t: string; c: char;\nbegin\n  read(a);\n  read(b);\n"
                 "  readln(s);\n  read(c);\n  readln(t);\n  writeln(a + b, '|', s, '|', c, '|', t);\nend.\n",
    "rlown.pas": "program RlOwn;\nvar f: text; a, b, c: string;\nbegin\n  assign(f, 'smoke/d.txt');\n  reset(f);\n"
                 "  readln(f, a);\n  readln(b);\n  readln(f, c);\n  close(f);\n  writeln(a, b, c);\nend.\n",
    "gen.pas": "program Gen;\nuses crt;\nvar i, r: integer;\nbegin\n  for i := 1 to 3 do\n  begin\n    delay(200);\n"
               "    r := ioresult;\n    writeln('line ', i);\n  end;\n  writeln('end');\nend.\n",
    "sc.clike": 'int main() {\n    int n;\n    scanf(n);\n    printf("clike got %d\\n", n * 2);\n    return 0;\n}\n',
    "rl1.clike": 'int main() {\n    str s;\n    readln(stdin, s);\n    printf("[%s]\\n", s);\n    return 0;\n}\n',
    "rlall.clike": 'int main() {\n    str s;\n    while (!eof(stdin)) {\n        readln(stdin, s);\n'
                   '        printf("[%s]\\n", s);\n    }\n    return 0;\n}\n',
    "rlmix.clike": 'int main() {\n    str s;\n    while (!eof(stdin)) {\n        scanf(s);\n'
                   '        printf("[%s]\\n", s);\n    }\n    return 0;\n}\n',
    "rlgen.clike": 'int main() {\n    str s;\n    int n;\n    n = 0;\n    while (!eof(stdin)) {\n'
                   '        readln(stdin, s);\n        n = n + 1;\n    }\n    printf("%d %s\\n", n, s);\n'
                   '    return 0;\n}\n',
    "rl.rea": 'class Main {}\n\nstr s;\nreadln(s);\nwriteln("rea got ", s);\n',
    "rlall.rea": 'class Main {}\n\nstr s;\nwhile (!eof(stdin)) {\n    readln(stdin, s);\n'
                 '    writeln("[", s, "]");\n}\n',
    "rl.aether": 'fn main() -> Void {\n    fx {\n        println("aether got ", read_line());\n'
                 '        println("then ", read_line());\n    }\n    ret;\n}\n',
    "all.aether": 'fn main() -> Void {\n    fx { print(read_all_stdin()); }\n    ret;\n}\n',
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
    "cp Examples/pascal/base/FindPrimes smoke/fp",
    "printf '10\\n' > smoke/ten",
    "printf '21\\n' > smoke/num",
    "printf 'pascal smoke/rl1.pas\\n' > smoke/rds.sh",
    "chmod +x smoke/ord_sb.sh smoke/sb.sh smoke/sb3.sh smoke/sbset.sh smoke/pos.sh smoke/posb.sh smoke/posw.sh smoke/fp",
    "mkdir -p smoke/rs/a/b",
    "printf 'x\\n' > smoke/rs/a/f",
    "printf 'y\\n' > smoke/rs/a/b/g",
    # The same names in two directories and the start directory, for the vm_ tests.
    "vmfiles() { rm -rf smoke/v1 smoke/v2; rm -f vm.* vmj.json; for t in 1 2 H; do if [ $t = H ]; then d=.; else d=smoke/v$t; mkdir -p $d/vmo; touch $d/vmo/only$t; fi; printf \"in$t\\n\" > $d/vm.in; printf \"ren$t\\n\" > $d/vm.ren; printf \"del$t\\n\" > $d/vm.del; printf '{\"n\":%s}\\n' \"${t#H}0\" > $d/vmj.json; done; }",
    "vmshow() { for f in smoke/v1/vm.* smoke/v2/vm.* vm.*; do echo \"$f: $(cat $f)\"; done; }",
    'vmctx() { cat smoke/v1/vm.out; [ -e vm.out ] && echo "wrong directory"; true; }',
]


class Known(NamedTuple):
    """A failure the release notes list: its issue and the output it gives (None: no result file)."""

    issue: str
    got: Optional[str]
    always: bool = True  # False: it comes and goes, so a pass is no news.


# The Known Issues in RELEASE_NOTES_V.1R32.md that these tests hit. A test here
# that fails any other way is an unexpected failure. When one is fixed, drop it
# here and from the next release's notes.
KNOWN = {}


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
        if state == "stalled":
            self._sample(pid)
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

    def _sample(self, pid: int) -> None:
        """Where each thread is: a stall leaves no crash report, and the app is stopped next."""
        path = Path(tempfile.gettempdir()) / f"pscal-stall-{self._last_step() or 'start'}-{pid}.txt"
        taken = subprocess.run(["/usr/bin/sample", str(pid), "2", "-file", str(path)],
                               capture_output=True, text=True)
        print(f"stack sample: {path}" if taken.returncode == 0 else f"no stack sample: {taken.stderr.strip()}")

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
