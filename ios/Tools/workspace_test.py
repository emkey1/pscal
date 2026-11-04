#!/usr/bin/env python3
"""Workspace end to end in the iOS Simulator, without touching the screen.

Drives the app through ios/Sources/App/WorkspaceTestHooks.swift, which is inert unless
the app is launched with PSCALI_WORKSPACE_TEST=1. This script does that. Commands typed
into a session write files under ~ (the container's Documents/home), so a pass means the
right shell really ran the command, not only that a view exists.

Usage:
  ios/Tools/workspace_test.py --app <PSCAL.app> --device <simulator UDID> [--out DIR]

Exit status 0 when every check passed; each failure is printed. A simulator that is not
booted is booted, and it is shut down again at the end.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

BUNDLE = "com.pscal.PSCAL"
NOTIFY = "com.pscal.PSCAL.workspace-test"


def run(*cmd, check=True, env=None):
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if check and result.returncode != 0:
        raise RuntimeError("%s failed (%d): %s" % (" ".join(cmd), result.returncode, result.stdout))
    return result.stdout.strip()


class App:
    def __init__(self, udid, app_path, out_dir):
        self.udid = udid
        self.app_path = app_path
        self.out_dir = out_dir
        self.seq = 0
        self.data = None

    def simctl(self, *args, **kw):
        return run("xcrun", "simctl", *args, **kw)

    def boot(self):
        self.simctl("bootstatus", self.udid, "-b")

    def shutdown(self):
        self.simctl("shutdown", self.udid, check=False)

    def install(self):
        self.simctl("uninstall", self.udid, BUNDLE, check=False)
        time.sleep(4)
        self.simctl("install", self.udid, self.app_path)
        # A location dialog would keep the app inactive, and so without a first responder.
        self.simctl("privacy", self.udid, "grant", "location-always", BUNDLE, check=False)
        self.data = self.simctl("get_app_container", self.udid, BUNDLE, "data")

    def launch(self, hooks=True):
        env = dict(os.environ)
        if hooks:
            env["SIMCTL_CHILD_PSCALI_WORKSPACE_TEST"] = "1"
        self.simctl("launch", "--terminate-running-process", self.udid, BUNDLE, env=env)

    def terminate(self):
        self.simctl("terminate", self.udid, BUNDLE, check=False)

    @property
    def hook_dir(self):
        return os.path.join(self.data, "tmp", "workspace-test")

    @property
    def home(self):
        return os.path.join(self.data, "Documents", "home")

    def wait_ready(self, timeout=60):
        deadline = time.time() + timeout
        ready = os.path.join(self.hook_dir, "ready.json")
        while time.time() < deadline:
            if os.path.exists(ready):
                return True
            time.sleep(0.25)
        return False

    def send(self, verb, rest="", timeout=20):
        self.seq += 1
        line = "%d %s %s\n" % (self.seq, verb, rest)
        os.makedirs(self.hook_dir, exist_ok=True)
        tmp = os.path.join(self.hook_dir, "cmd.tmp")
        with open(tmp, "w") as handle:
            handle.write(line)
        os.replace(tmp, os.path.join(self.hook_dir, "cmd.txt"))
        self.simctl("spawn", self.udid, "notifyutil", "-p", NOTIFY)
        result = os.path.join(self.hook_dir, "result-%d.json" % self.seq)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(result):
                with open(result) as handle:
                    return json.load(handle)
            time.sleep(0.1)
        return None

    def dump(self):
        return self.send("dump")

    def screenshot(self, name):
        if self.out_dir:
            self.simctl("io", self.udid, "screenshot", os.path.join(self.out_dir, name + ".png"), check=False)

    def wait_file(self, name, tab=None, command=None, timeout=45):
        """Waits for ~/name, typing the command again every 15 s if the shell wasn't ready."""
        path = os.path.join(self.home, name)
        deadline = time.time() + timeout
        next_type = 0
        while time.time() < deadline:
            if os.path.exists(path) and os.path.getsize(path) > 0:
                time.sleep(0.2)
                with open(path) as handle:
                    return handle.read().strip()
            if command and time.time() >= next_type:
                self.send("type", "%s %s\\n" % (tab, command))
                next_type = time.time() + 15
            time.sleep(0.25)
        return None

    def clear_file(self, name):
        path = os.path.join(self.home, name)
        if os.path.exists(path):
            os.remove(path)


class Results:
    def __init__(self):
        self.failures = []
        self.passed = 0

    def check(self, name, ok, detail=""):
        if ok:
            self.passed += 1
            print("PASS", name, flush=True)
        else:
            self.failures.append(name)
            print("FAIL", name, detail, flush=True)
        return ok


def wait_for(app, predicate, timeout=20):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = app.dump()
        if last and predicate(last):
            return last
        time.sleep(0.3)
    return last


def tab_info(dump, tab_id):
    for tab in dump["tabs"]:
        if tab["id"] == tab_id:
            return tab
    return {}


def window_info(dump, window_id):
    for window in dump.get("windows", []):
        if window["id"] == window_id:
            return window
    return {}


def hosted_info(window, tab_id):
    for tab in window.get("tabs", []):
        if tab["id"] == tab_id:
            return tab
    return {}


def size_of(app, tab, name):
    app.clear_file(name)
    text = app.wait_file(name, tab, "stty size > ~/%s" % name)
    if not text:
        return None
    parts = text.split()
    return (int(parts[0]), int(parts[1])) if len(parts) == 2 else None


def run_tests(app, results):
    app.launch()
    results.check("hooks answer", app.wait_ready(), "no ready.json")
    first = wait_for(app, lambda d: d.get("mode") == "tabs" and d["tabs"])
    results.check("starts in tabs", first and first["mode"] == "tabs", first)
    root_tab = first["tabs"][0]["id"]

    # Into Workspace: the root session gets a window and keeps running.
    app.send("workspace")
    dump = wait_for(app, lambda d: len(d.get("windows", [])) == 1 and tab_info(d, root_tab).get("webAttached"))
    results.check("root session gets a window", dump and len(dump.get("windows", [])) == 1, dump)
    root_win = dump["windows"][0]["id"]
    results.check("root window has no close button", not window_info(dump, root_win)["closable"])
    dump = wait_for(app, lambda d: d.get("keyInputFocused"))
    results.check("root terminal takes the keyboard", dump and dump["keyInputFocused"], dump)
    root_pid = app.wait_file("wt_root.pid", root_tab, "echo $$ > ~/wt_root.pid")
    results.check("root shell runs commands", bool(root_pid), root_pid)

    # A second window: both on screen, one with the keyboard.
    app.send("open-terminal")
    dump = wait_for(app, lambda d: len(d.get("windows", [])) == 2 and len(d["tabs"]) == 2)
    results.check("New Terminal opens a window", dump and len(dump.get("windows", [])) == 2, dump)
    other_tab = [t["id"] for t in dump["tabs"] if t["id"] != root_tab][0]
    other_win = [w["id"] for w in dump["windows"] if w["id"] != root_win][0]
    dump = wait_for(app, lambda d: tab_info(d, root_tab).get("webAttached") and tab_info(d, other_tab).get("webAttached")
                    and tab_info(d, other_tab).get("loaded"))
    results.check("both terminals stay attached", dump and tab_info(dump, root_tab).get("webAttached")
                  and tab_info(dump, other_tab).get("webAttached"), dump)
    results.check("new window has the keyboard", window_info(dump, other_win).get("focused")
                  and dump["selectedId"] == other_tab
                  and not hosted_info(window_info(dump, root_win), root_tab).get("focused"), dump)
    results.check("new window is in front", window_info(dump, other_win).get("front"), dump)
    other_pid = app.wait_file("wt_other.pid", other_tab, "echo $$ > ~/wt_other.pid")
    results.check("second shell runs commands", bool(other_pid), other_pid)
    results.check("the shells are different", other_pid != root_pid, (root_pid, other_pid))
    app.screenshot("two-windows")

    # A session behind another still runs and its grid follows its own window.
    late = app.wait_file("wt_root2", root_tab, "echo behind > ~/wt_root2")
    results.check("a window behind another keeps running", late == "behind", late)
    root_size = size_of(app, root_tab, "wt_root.size")
    app.send("frame", "%d 0.05 0.5 0.45 0.3" % other_win)
    time.sleep(1.5)
    small = size_of(app, other_tab, "wt_other.small")
    app.send("frame", "%d 0.05 0.1 0.9 0.5" % other_win)
    time.sleep(1.5)
    large = size_of(app, other_tab, "wt_other.large")
    results.check("resizing a window resizes its terminal", small and large and large[1] > small[1], (small, large))
    root_size2 = size_of(app, root_tab, "wt_root.size2")
    results.check("the other terminal keeps its size", root_size == root_size2, (root_size, root_size2))

    # Focus moves with the window.
    app.send("focus", str(root_win))
    dump = wait_for(app, lambda d: d["selectedId"] == root_tab and window_info(d, root_win).get("front"))
    results.check("focus brings a window forward", dump and dump["selectedId"] == root_tab
                  and window_info(dump, root_win).get("focused") and not window_info(dump, other_win).get("focused"), dump)
    dump = wait_for(app, lambda d: d.get("keyInputFocused"))
    results.check("the focused terminal has the keyboard", dump and dump["keyInputFocused"], dump)
    app.send("focus-desktop")
    dump = wait_for(app, lambda d: not d.get("keyInputFocused") and not any(w["focused"] for w in d["windows"]))
    results.check("a tap on the desktop takes the keyboard away", dump and not dump["keyInputFocused"], dump)
    still = app.wait_file("wt_root3", root_tab, "echo unfocused > ~/wt_root3")
    results.check("nothing focused, sessions still run", still == "unfocused", still)

    # Tabs in a window.
    app.send("new-tab", str(other_win))
    dump = wait_for(app, lambda d: len(window_info(d, other_win).get("tabs", [])) == 2)
    results.check("New Tab adds to the window", dump and len(window_info(dump, other_win).get("tabs", [])) == 2
                  and len(dump["windows"]) == 2, dump)
    tab3 = window_info(dump, other_win)["selectedTab"]
    results.check("the new tab is selected", tab3 not in (root_tab, other_tab) and dump["selectedId"] == tab3, dump)
    dump = wait_for(app, lambda d: tab_info(d, tab3).get("webAttached") and not tab_info(d, other_tab).get("webAttached"))
    results.check("only the shown tab is attached", dump and tab_info(dump, tab3).get("webAttached")
                  and not tab_info(dump, other_tab).get("webAttached")
                  and not hosted_info(window_info(dump, other_win), other_tab).get("visible"), dump)
    hidden = app.wait_file("wt_hidden", other_tab, "echo hidden > ~/wt_hidden")
    results.check("a hidden tab keeps running", hidden == "hidden", hidden)
    app.send("select-tab", "%d 0" % other_win)
    dump = wait_for(app, lambda d: tab_info(d, other_tab).get("webAttached") and d["selectedId"] == other_tab)
    results.check("selecting a tab shows it", dump and dump["selectedId"] == other_tab
                  and tab_info(dump, other_tab).get("webAttached"), dump)
    app.send("close-tab", tab3)
    dump = wait_for(app, lambda d: len(window_info(d, other_win).get("tabs", [])) == 1 and len(d["tabs"]) == 2)
    results.check("closing a tab keeps the window", dump and len(dump["windows"]) == 2
                  and len(dump["tabs"]) == 2, dump)

    # Zoom and back.
    before = window_info(app.dump(), other_win)["frame"]
    app.send("zoom", str(other_win))
    dump = wait_for(app, lambda d: window_info(d, other_win).get("zoomed"))
    surface = dump["surface"]
    frame = window_info(dump, other_win)["frame"]
    results.check("zoom fills the desktop", frame[2] == surface[0] and abs(frame[1] + frame[3] - surface[1]) < 1, (frame, surface))
    app.send("zoom", str(other_win))
    dump = wait_for(app, lambda d: not window_info(d, other_win).get("zoomed"))
    results.check("zoom comes back", window_info(dump, other_win)["frame"] == before, (before, window_info(dump, other_win)))

    # Ending a shell closes its tab, and the window with its last one.
    app.send("type", "%s exit\\n" % other_tab)
    dump = wait_for(app, lambda d: len(d.get("windows", [])) == 1, timeout=30)
    results.check("exit closes the window", dump and len(dump.get("windows", [])) == 1 and len(dump["tabs"]) == 1, dump)
    results.check("focus falls back to what is left", dump and dump["selectedId"] == root_tab
                  and window_info(dump, root_win).get("focused"), dump)

    # The window's × ends the session.
    app.send("open-terminal")
    dump = wait_for(app, lambda d: len(d.get("windows", [])) == 2)
    closing = [w["id"] for w in dump["windows"] if w["id"] != root_win][0]
    app.send("close", str(closing))
    dump = wait_for(app, lambda d: len(d.get("windows", [])) == 1 and len(d["tabs"]) == 1)
    results.check("the window's close ends its session", dump and len(dump["tabs"]) == 1, dump)

    # Back and forth: moving between tabs and Workspace must not stop a session's output.
    for round_number in range(3):
        app.send("tabs")
        dump = wait_for(app, lambda d: d["mode"] == "tabs" and tab_info(d, root_tab).get("webAttached"))
        results.check("back to tabs %d" % round_number, dump and dump["mode"] == "tabs", dump)
        name = "wt_tabs%d" % round_number
        text = app.wait_file(name, root_tab, "echo t > ~/%s" % name)
        results.check("the session runs in tabs %d" % round_number, text == "t", text)
        app.send("workspace")
        dump = wait_for(app, lambda d: d["mode"] == "workspace" and len(d.get("windows", [])) == 1
                        and tab_info(d, root_tab).get("webAttached"))
        results.check("into Workspace %d" % round_number, dump and dump["mode"] == "workspace", dump)
        name = "wt_ws%d" % round_number
        text = app.wait_file(name, root_tab, "echo w > ~/%s" % name)
        results.check("the session runs in Workspace %d" % round_number, text == "w", text)
    app.screenshot("after-round-trips")


def run_inert_control(app, results):
    """A launch without the variable must not answer. SIMCTL_CHILD_ variables stay in the
    environment until the simulator shuts down, so this needs a shutdown first."""
    app.terminate()
    app.shutdown()
    app.boot()
    if os.path.isdir(app.hook_dir):
        shutil.rmtree(app.hook_dir)
    app.launch(hooks=False)
    time.sleep(8)
    answer = app.send("dump", timeout=6)
    results.check("hooks are inert without the variable", answer is None, answer)
    app.terminate()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--out", default=None, help="directory for screenshots")
    parser.add_argument("--keep", action="store_true", help="leave the simulator booted")
    args = parser.parse_args()
    out_dir = args.out or tempfile.mkdtemp(prefix="workspace-test-")
    os.makedirs(out_dir, exist_ok=True)

    app = App(args.device, args.app, out_dir)
    results = Results()
    app.boot()
    try:
        app.install()
        run_tests(app, results)
        run_inert_control(app, results)
    finally:
        app.terminate()
        if not args.keep:
            app.shutdown()
    print("%d passed, %d failed" % (results.passed, len(results.failures)))
    print("screenshots in", out_dir)
    for name in results.failures:
        print("FAILED:", name)
    return 1 if results.failures else 0


if __name__ == "__main__":
    sys.exit(main())
