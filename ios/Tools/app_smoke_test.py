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
    ("exsh_c", f"exsh -c 'echo inner' {R}", "inner\n"),
    ("exsh_script", f"exsh smoke/s.sh {R}", "script\n"),
    ("sh_script", f"sh smoke/s.sh {R}", "script\n"),
    ("sh_c", f"sh -c 'echo inner | cat' {R}", "inner\n"),
    ("md_view", f"printf '# t\\n' | md 2>&1 | wc -l {R}", None),
    ("env_var", f"X=1 env | grep -c '^X=1$' {R}", "1\n"),
    ("pascal_run", f"pascal smoke/hello.pas {R}", "pascal ok 42\n"),
    ("clike_run", f"clike smoke/hello.clike {R}", "clike ok 42\n"),
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
    # In a full run this one captures bytecode in its output, which the release
    # notes do not list, so it has no expected output yet.
    ("tiny_compile", f"tiny smoke/max.tiny smoke/max.pbc {R}", None),
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
    ("find_abs", f"find /home/smoke/sub2 -type f {R}", "/home/smoke/sub2/x\n"),
    ("du_abs", f"du -s /home/smoke/sub2 | wc -l {R}", "1\n"),
    ("stat_abs", f"stat -c %s /home/smoke/d.txt {R}", "6\n"),
    ("sed_abs", f"sed s/a/z/ /home/smoke/d.txt {R}", "b\nz\nc\n"),
    ("grep_abs", f"grep a /home/smoke/d.txt {R}", "a\n"),
    ("tar_abs", f"tar cf /home/smoke/y.tar -C /home/smoke sub2; tar tf /home/smoke/y.tar | head -1 {R}", "sub2/\n"),
    ("realpath_v", f"realpath /home/smoke/d.txt {R}", "/home/smoke/d.txt\n"),
    # As tiny_compile.
    ("tiny_full", f"tiny smoke/cd.tiny /home/smoke/cd3.pbc {R}; ls smoke/cd3.pbc {A}", None),
]
BY_NAME = {test[0]: test for test in TESTS}

# Written from the host, since they are awkward to quote in a shell script.
FIXTURES = {
    "hello.pas": "program Hello;\nbegin\n  writeln('pascal ok ', 6*7);\nend.\n",
    "hello.clike": 'int main() {\n    printf("clike ok %d\\n", 6*7);\n    return 0;\n}\n',
    "hello.rea": 'class Main {}\n\nwriteln("rea ok ", 6*7);\n',
    "hello.aether": '@pure\nfn answer(a: Int, b: Int) -> Int {\n    ret a * b;\n}\n\nfn main() -> Void {\n    fx { println("aether ok ", answer(6, 7)); }\n    ret;\n}\n',
    "slow.pas": "program Slow;\nuses crt;\nbegin\n  delay(800);\n  writeln('slow done');\nend.\n",
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
    "printf '#!/bin/exsh\\nset -- x y\\necho $1 $2\\n' > smoke/sbset.sh",
    "printf '#!/bin/exsh\\necho $1 $2\\n' > smoke/pos.sh",
    "printf '#!/bin/exsh\\necho ${1}/${2}\\n' > smoke/posb.sh",
    "printf '#!/bin/exsh\\necho $1.txt\\n' > smoke/posw.sh",
    "printf 'x := 6 * 7;\\nwrite x\\n' > smoke/p.tiny",
    "chmod +x smoke/ord_sb.sh smoke/sb.sh smoke/sbset.sh smoke/pos.sh smoke/posb.sh smoke/posw.sh",
]


class Known(NamedTuple):
    """A failure the release notes list: its issue and the output it gives (None: no result file)."""

    issue: str
    got: Optional[str]
    always: bool = True  # False: it comes and goes, so a pass is no news.


# The Known Issues in RELEASE_NOTES_V.1R31.md that these tests hit. A test here
# that fails any other way is an unexpected failure. When one is fixed, drop it
# here and from the next release's notes.
KNOWN = {
    "awk_lt": Known("`awk '...' < file` can come back empty in a long run", "", always=False),
    "split_ok": Known("`split -l 1 file prefix; cat prefixaa` can find an empty first piece", "", always=False),
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
