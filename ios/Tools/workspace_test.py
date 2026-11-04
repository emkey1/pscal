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
import struct
import subprocess
import sys
import tempfile
import time
import zlib

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

    run_desktop_tests(app, results, root_tab, root_win_after_round_trips(app, root_tab))


def root_win_after_round_trips(app, root_tab):
    dump = app.dump()
    for window in dump.get("windows", []):
        if hosted_info(window, root_tab):
            return window["id"]
    return None


def run_desktop_tests(app, results, root_tab, root_win):
    """Virtual Desktops, the Desktops applet, and an arrangement across a relaunch."""
    app.send("desktops", "3")
    dump = wait_for(app, lambda d: d.get("desktopCount") == 3)
    results.check("Desktops can be added", dump and dump["desktopCount"] == 3, dump)
    results.check("the active Desktop stays put", dump and dump["activeDesktop"] == 0, dump)

    # A window on another Desktop: hidden, detached, still running.
    app.send("open-terminal")
    dump = wait_for(app, lambda d: len(d.get("windows", [])) == 2)
    other_win = [w["id"] for w in dump["windows"] if w["id"] != root_win][0]
    other_tab = window_info(dump, other_win)["selectedTab"]
    app.send("assign", "%d 1" % other_win)
    dump = wait_for(app, lambda d: window_info(d, other_win).get("hidden"))
    results.check("assigning to another Desktop hides a window", dump and window_info(dump, other_win).get("hidden")
                  and window_info(dump, other_win).get("desktop") == 1, dump)
    results.check("focus falls back on this Desktop", dump and window_info(dump, root_win).get("focused")
                  and dump["selectedId"] == root_tab, dump)
    dump = wait_for(app, lambda d: not tab_info(d, other_tab).get("webAttached"))
    results.check("a window on another Desktop detaches its terminal", dump
                  and not tab_info(dump, other_tab).get("webAttached"), dump)
    text = app.wait_file("wt_desk_hidden", other_tab, "echo hidden > ~/wt_desk_hidden")
    results.check("a session on another Desktop keeps running", text == "hidden", text)

    app.send("switch", "1")
    dump = wait_for(app, lambda d: d.get("activeDesktop") == 1 and not window_info(d, other_win).get("hidden"))
    results.check("switching shows that Desktop's windows", dump and not window_info(dump, other_win)["hidden"]
                  and window_info(dump, root_win)["hidden"], dump)
    results.check("switching focuses that Desktop's window", dump and window_info(dump, other_win).get("focused")
                  and dump["selectedId"] == other_tab, dump)
    dump = wait_for(app, lambda d: tab_info(d, other_tab).get("webAttached") and tab_info(d, other_tab).get("loaded"))
    results.check("its terminal attaches again", dump and tab_info(dump, other_tab).get("webAttached"), dump)
    app.screenshot("desktop-2")

    app.send("switch", "2")
    dump = wait_for(app, lambda d: d.get("activeDesktop") == 2)
    results.check("an empty Desktop has nothing focused", dump and not any(w["focused"] for w in dump["windows"])
                  and not dump.get("keyInputFocused"), dump)
    app.send("select-session", root_tab)
    dump = wait_for(app, lambda d: d.get("activeDesktop") == 0)
    results.check("selecting a session goes to its Desktop", dump and dump["activeDesktop"] == 0
                  and window_info(dump, root_win).get("focused"), dump)

    # The Desktops applet is one window, shared by every Desktop.
    app.send("open-tool", "workspaces")
    dump = wait_for(app, lambda d: any(w["tool"] == "workspaces" for w in d.get("windows", [])))
    results.check("the Desktops applet opens", dump and any(w["tool"] == "workspaces" for w in dump["windows"]), dump)
    tool_win = [w["id"] for w in dump["windows"] if w["tool"] == "workspaces"][0]
    results.check("an applet in front has no terminal with the keyboard", dump and not dump.get("keyInputFocused")
                  or wait_for(app, lambda d: not d.get("keyInputFocused")), dump)
    app.screenshot("desktops-applet")
    app.send("switch", "1")
    dump = wait_for(app, lambda d: d.get("activeDesktop") == 1)
    results.check("the Desktops applet shows on every Desktop", dump and not window_info(dump, tool_win)["hidden"], dump)
    app.send("open-tool", "workspaces")
    dump = wait_for(app, lambda d: d.get("activeDesktop") == 1)
    results.check("opening it again keeps one window", dump and sum(w["tool"] == "workspaces" for w in dump["windows"]) == 1, dump)

    # Save and Restore: a Save is a real change, and Restore moves windows rather than ending shells.
    dump = app.dump()
    results.check("nothing is saved yet", dump and not dump["saved"], dump)
    app.send("frame", "%d 0.1 0.2 0.5 0.4" % other_win)
    time.sleep(0.5)
    app.send("save-arrangement")
    dump = wait_for(app, lambda d: d.get("saved"))
    results.check("Save records the arrangement", dump and dump["saved"], dump)
    saved_frame = window_info(dump, other_win)["frame"]
    app.send("assign", "%d 2" % other_win)
    dump = wait_for(app, lambda d: window_info(d, other_win).get("desktop") == 2)
    results.check("moving a window to another Desktop reads as unsaved", dump and not dump["saved"], dump)
    app.send("frame", "%d 0.3 0.1 0.4 0.3" % other_win)
    time.sleep(0.5)
    app.send("restore-arrangement")
    dump = wait_for(app, lambda d: window_info(d, other_win).get("desktop") == 1 and d.get("saved"))
    results.check("Restore moves the window back", dump and window_info(dump, other_win)["desktop"] == 1
                  and window_info(dump, other_win)["frame"] == saved_frame, dump)
    results.check("Restore keeps the session", dump and tab_info(dump, other_tab), dump)
    pid_before = app.wait_file("wt_desk_pid1", other_tab, "echo $$ > ~/wt_desk_pid1")
    pid_after = app.wait_file("wt_desk_pid2", other_tab, "echo $$ > ~/wt_desk_pid2")
    results.check("the shell is the same one after Restore", pid_before and pid_before == pid_after, (pid_before, pid_after))

    # A locked Desktop stays; the first always does. Removing one closes what it holds.
    app.send("remove-desktop", "0")
    dump = wait_for(app, lambda d: d.get("desktopCount") == 3)
    results.check("the first Desktop can't be removed", dump and dump["desktopCount"] == 3, dump)
    app.send("remove-desktop", "2")
    dump = wait_for(app, lambda d: d.get("desktopCount") == 2)
    results.check("removing a Desktop shrinks the count", dump and dump["desktopCount"] == 2, dump)

    # Back to tabs and back: the arrangement stays for this run.
    app.send("tabs")
    wait_for(app, lambda d: d["mode"] == "tabs")
    app.send("workspace")
    dump = wait_for(app, lambda d: d["mode"] == "workspace" and d.get("desktopCount") == 2 and len(d.get("windows", [])) >= 2)
    results.check("coming back from tabs keeps the arrangement", dump and dump["desktopCount"] == 2
                  and any(hosted_info(w, other_tab) and w["desktop"] == 1 for w in dump["windows"]), dump)

    # A relaunch: fresh shells where the saved ones were. Saved above with 3 Desktops.
    app.send("save-arrangement")
    wait_for(app, lambda d: d.get("saved"))
    app.terminate()
    time.sleep(2)
    app.launch()
    time.sleep(1)
    results.check("hooks answer after a relaunch", app.wait_ready(), "no ready.json")
    first = wait_for(app, lambda d: d.get("mode") == "tabs" and d["tabs"])
    app.send("workspace")
    dump = wait_for(app, lambda d: d.get("mode") == "workspace" and len(d.get("windows", [])) >= 2)
    results.check("a relaunch restores the windows", dump and len([w for w in dump["windows"] if not w["tool"]]) == 2, dump)
    results.check("a relaunch restores the Desktops", dump and dump["desktopCount"] == 2, dump)
    results.check("a relaunch puts a fresh shell where the saved one was", dump
                  and any(not w["tool"] and w["desktop"] == 1 for w in dump["windows"]), dump)
    results.check("a relaunch reads as saved", dump and dump["saved"], dump)
    app.screenshot("after-relaunch")

    run_applet_tests(app, results)


APPLETS = ["clock", "monitor", "networks", "status", "launcher", "shortcuts", "sessions", "themes", "settings", "diagnostics",
           "filemanager", "motepad", "markdown", "imageviewer", "storage"]


def inside(frame, surface):
    return frame[0] >= -0.5 and frame[1] >= -0.5 and frame[0] + frame[2] <= surface[0] + 0.5 and frame[1] + frame[3] <= surface[1] + 0.5


def run_applet_tests(app, results):
    """Each applet at its opening size, then what the kit gives them all, then `ws`."""
    dump = wait_for(app, lambda d: d.get("mode") == "workspace")
    results.check("the theme draws a wallpaper", dump and dump.get("hasWallpaper"), dump)
    results.check("Graphite is the default theme", dump and dump.get("theme") == "graphite", dump)
    root = [t["id"] for t in dump["tabs"] if t["root"]][0]

    for tool in APPLETS:
        before = len(app.dump().get("windows", []))
        app.send("open-or-focus", tool)
        dump = wait_for(app, lambda d: any(w["tool"] == tool for w in d.get("windows", [])))
        results.check("%s opens" % tool, dump and any(w["tool"] == tool for w in dump.get("windows", [])), dump)
        if not dump:
            continue
        window = [w for w in dump["windows"] if w["tool"] == tool][0]
        results.check("%s opens in front" % tool, window["front"] and window["focused"], window)
        results.check("%s fits the desktop" % tool, inside(window["frame"], dump["surface"]), (window, dump["surface"]))
        results.check("%s adds one window" % tool, len(dump["windows"]) == before + 1, dump)
        time.sleep(1)
        app.screenshot("applet-%s" % tool)
        app.send("open-or-focus", tool)
        dump = wait_for(app, lambda d: sum(w["tool"] == tool for w in d.get("windows", [])) == 1)
        results.check("%s keeps one window when asked again" % tool,
                      dump and sum(w["tool"] == tool for w in dump["windows"]) == 1, dump)
        app.send("close", str(window["id"]))
        dump = wait_for(app, lambda d: not any(w["tool"] == tool for w in d.get("windows", [])))
        results.check("%s closes" % tool, dump and not any(w["tool"] == tool for w in dump["windows"]), dump)

    # Every shell must have outlived all of that.
    text = app.wait_file("wt_after_applets", root, "echo alive > ~/wt_after_applets")
    results.check("the shell runs after the applets", text == "alive", text)

    # A theme reaches everything open, and it's saved.
    app.send("open-or-focus", "monitor")
    dump = wait_for(app, lambda d: any(w["tool"] == "monitor" for w in d.get("windows", [])))
    monitor = [w for w in dump["windows"] if w["tool"] == "monitor"][0]
    rings_size = monitor["frame"][3]
    for theme in ["aurora", "solstice", "graphite"]:
        app.send("theme", theme)
        dump = wait_for(app, lambda d: d.get("theme") == theme)
        results.check("theme %s applies" % theme, dump and dump["theme"] == theme, dump)
        time.sleep(0.5)
        app.screenshot("theme-%s" % theme)
    app.send("gauge", "bar")
    dump = wait_for(app, lambda d: window_info(d, monitor["id"])["frame"][3] < rings_size)
    results.check("Monitor resizes for bar gauges", dump and window_info(dump, monitor["id"])["frame"][3] < rings_size, dump)
    app.screenshot("monitor-bars")
    app.send("gauge", "ring")
    dump = wait_for(app, lambda d: window_info(d, monitor["id"])["frame"][3] == rings_size)
    results.check("Monitor resizes back for rings", dump and window_info(dump, monitor["id"])["frame"][3] == rings_size, dump)

    # The text size is per window, grows Monitor's window, and is saved with the arrangement.
    app.send("text-scale", "%d 1" % monitor["id"])
    dump = wait_for(app, lambda d: window_info(d, monitor["id"]).get("textScale", 1) > 1)
    results.check("the text size steps up", dump and window_info(dump, monitor["id"])["textScale"] > 1, dump)
    results.check("Monitor's window grows with its text", dump and window_info(dump, monitor["id"])["frame"][2] > monitor["frame"][2], dump)
    scale = window_info(dump, monitor["id"])["textScale"]
    app.screenshot("monitor-large-text")
    app.send("save-arrangement")
    wait_for(app, lambda d: d.get("saved"))

    # A Launcher shortcut: a {token} opens an applet, a command runs in a new terminal.
    app.send("add-shortcut", "probe echo launched > ~/wt_launcher")
    dump = wait_for(app, lambda d: d.get("shortcuts") == 1)
    results.check("a shortcut is added", dump and dump["shortcuts"] == 1, dump)
    app.send("open-or-focus", "launcher")
    wait_for(app, lambda d: any(w["tool"] == "launcher" for w in d.get("windows", [])))
    time.sleep(0.5)
    app.screenshot("launcher-one-shortcut")
    tabs_before = len(app.dump()["tabs"])
    app.clear_file("wt_launcher")
    app.send("run-shortcut", "echo launched > ~/wt_launcher")
    dump = wait_for(app, lambda d: len(d["tabs"]) == tabs_before + 1)
    results.check("a shortcut opens a terminal", dump and len(dump["tabs"]) == tabs_before + 1, dump)
    text = app.wait_file("wt_launcher", timeout=30)
    results.check("a shortcut's command runs once the prompt is up", text == "launched", text)
    app.send("run-shortcut", "{clock}")
    dump = wait_for(app, lambda d: any(w["tool"] == "clock" for w in d.get("windows", [])))
    results.check("a {token} opens its applet", dump and any(w["tool"] == "clock" for w in dump["windows"]), dump)

    # `ws`, from a real shell.
    app.clear_file("wt_ws_status")
    text = app.wait_file("wt_ws_status", root, "ws status > ~/wt_ws_status")
    results.check("ws status says hosted", bool(text) and "hosted=1" in text and "clock" in text, text)
    app.send("close", str([w["id"] for w in app.dump()["windows"] if w["tool"] == "clock"][0]))
    wait_for(app, lambda d: not any(w["tool"] == "clock" for w in d.get("windows", [])))
    app.send("type", "%s ws open clock\\n" % root)
    dump = wait_for(app, lambda d: any(w["tool"] == "clock" for w in d.get("windows", [])))
    results.check("ws open opens an applet", dump and any(w["tool"] == "clock" for w in dump["windows"]), dump)
    app.send("focus", str([w["id"] for w in dump["windows"] if hosted_info(w, root)][0]))
    app.clear_file("wt_ws_applets")
    text = app.wait_file("wt_ws_applets", root, "ws applets > ~/wt_ws_applets")
    results.check("ws applets lists it", bool(text) and " clock " in text, text)
    app.clear_file("wt_ws_bad")
    text = app.wait_file("wt_ws_bad", root, "ws open nosuchtool 2> ~/wt_ws_bad")
    results.check("ws open refuses an unknown tool", bool(text) and "no such tool" in text, text)

    # And it answers honestly from tabs, without hanging the shell.
    app.send("tabs")
    wait_for(app, lambda d: d["mode"] == "tabs")
    app.clear_file("wt_ws_tabs")
    text = app.wait_file("wt_ws_tabs", root, "ws status > ~/wt_ws_tabs")
    results.check("ws status says not hosted in tabs", bool(text) and "hosted=0" in text, text)
    app.clear_file("wt_ws_tabs2")
    text = app.wait_file("wt_ws_tabs2", root, "ws open clock 2> ~/wt_ws_tabs2")
    results.check("ws open refuses in tabs", bool(text) and "not under Workspace" in text, text)
    app.send("workspace")
    wait_for(app, lambda d: d["mode"] == "workspace")

    # A relaunch keeps the theme's choice, the applet and its text size.
    app.send("theme", "solstice")
    wait_for(app, lambda d: d.get("theme") == "solstice")
    app.terminate()
    time.sleep(2)
    app.launch()
    time.sleep(1)
    results.check("hooks answer after the applet relaunch", app.wait_ready(), "no ready.json")
    wait_for(app, lambda d: d.get("mode") == "tabs" and d["tabs"])
    app.send("workspace")
    dump = wait_for(app, lambda d: d.get("mode") == "workspace" and any(w["tool"] == "monitor" for w in d.get("windows", [])))
    results.check("a relaunch keeps the theme", dump and dump.get("theme") == "solstice", dump)
    window = next((w for w in (dump or {}).get("windows", []) if w["tool"] == "monitor"), None)
    results.check("a relaunch restores the applet", window is not None, dump)
    results.check("a relaunch restores its text size", window and window["textScale"] == scale, window)
    app.screenshot("after-applet-relaunch")


UNICODE_NAME = "héllo wörld ☃ 日本語.txt"


def tiny_png(width=64, height=48):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    row = b"\x00" + bytes(x % 256 for x in range(width * 3))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(row * height)) + chunk(b"IEND", b""))


def make_file_tree(base):
    """Everything the file applets must survive, made from outside the app."""
    if os.path.isdir(base):
        shutil.rmtree(base)
    os.makedirs(base)

    def put(name, data):
        path = os.path.join(base, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)

    put("plain.txt", b"hello\n")
    put("other.txt", b"other\n")
    put(UNICODE_NAME, "snow ☃\n".encode())
    put("gone-soon.txt", b"keep me\n")
    put("note.md", b"# Title\n\nSome *text* and a [link](plain.txt).\n")
    put("pic.png", tiny_png())
    put("zero.png", b"")
    put("corrupt.png", b"\x89PNG\r\n\x1a\n" + b"not an image" * 64)
    put("latin1.txt", b"caf\xe9\n")
    put("large.txt", b"a" * (9 * 1024 * 1024))
    put("doomed/inside.txt", b"x")
    put("mntsrc/held.txt", b"held\n")
    os.symlink("plain.txt", os.path.join(base, "link.txt"))
    os.symlink("nowhere", os.path.join(base, "broken"))
    os.symlink("loop", os.path.join(base, "loop"))
    os.makedirs(os.path.join(base, "big"))
    for index in range(10000):
        open(os.path.join(base, "big", "f%05d" % index), "w").close()
    deep = "/".join(["d" * 38] * 20)
    put(deep + "/leaf.txt", b"leaf\n")
    return deep


def applet_windows(dump, tool):
    return [w for w in (dump or {}).get("windows", []) if w["tool"] == tool]


def applet_window(dump, serial):
    return next((w for w in (dump or {}).get("windows", []) if w["id"] == serial), None)


def open_file(app, tool, path):
    """Opens a path in a new window of the tool, and returns that window."""
    before = {w["id"] for w in applet_windows(app.dump(), tool)}
    app.send("open-file", "%s %s" % (tool, path))
    dump = wait_for(app, lambda d: any(w["id"] not in before for w in applet_windows(d, tool)))
    return next((w for w in applet_windows(dump, tool) if w["id"] not in before), None)


def wait_applet(app, serial, predicate, timeout=20):
    dump = wait_for(app, lambda d: applet_window(d, serial) and predicate(applet_window(d, serial)["applet"]), timeout)
    window = applet_window(dump, serial)
    return window["applet"] if window else None


def file_op(app, token, operation, path=""):
    app.send("file-op", "%s %s %s" % (token, operation, path))
    dump = wait_for(app, lambda d: d.get("fileResult", {}).get("token") == token, 30)
    return (dump or {}).get("fileResult", {})


def close_tools(app, *tools):
    for tool in tools:
        for window in applet_windows(app.dump(), tool):
            app.send("close", str(window["id"]))


def run_file_tests(app, results):
    """The file applets on what makes file code fail, and the service's refusals."""
    base = os.path.join(app.home, "wt_files")
    deep = make_file_tree(base)
    dump = wait_for(app, lambda d: d.get("mode") == "workspace")
    root = [t["id"] for t in dump["tabs"] if t["root"]][0]
    app.clear_file("wt_home")
    home = app.wait_file("wt_home", root, 'echo "$HOME" > ~/wt_home')
    results.check("the shell says where home is", bool(home), home)
    shown = "%s/wt_files" % home

    # File Manager.
    close_tools(app, "filemanager")
    wait_for(app, lambda d: not applet_windows(d, "filemanager"))
    app.send("open-or-focus", "filemanager")
    dump = wait_for(app, lambda d: applet_windows(d, "filemanager") and not applet_windows(d, "filemanager")[0]["applet"].get("loading"))
    files = applet_windows(dump, "filemanager")[0]
    results.check("File Manager starts at home", files["applet"]["path"] == home, files["applet"])
    fm = files["id"]

    def go(path, predicate=lambda state: True, timeout=30):
        app.send("fm-navigate", "%d %s" % (fm, path))
        return wait_applet(app, fm, lambda s: s["path"] == path and not s["loading"] and predicate(s), timeout)

    state = go(shown)
    names = set((state or {}).get("names", []))
    for name in ["plain.txt", UNICODE_NAME, "link.txt", "broken", "loop", "big", "doomed"]:
        results.check("the listing shows %s" % name, name in names, sorted(names))
    app.screenshot("files-listing")
    began = time.time()
    state = go("%s/big" % shown, lambda s: s["count"] == 10000)
    results.check("10,000 entries list in time", state and state["count"] == 10000, state)
    print("  10,000 entries: %.1f s" % (time.time() - began), flush=True)
    results.check("the app answers while they're shown", app.dump() is not None)
    app.screenshot("files-big")
    state = go("%s/%s" % (shown, deep), lambda s: s["count"] == 1)
    results.check("a deep path lists", state and state["names"] == ["leaf.txt"], state)
    state = go("%s/doomed" % shown, lambda s: s["count"] == 1)
    results.check("a folder about to go lists", state and state["count"] == 1, state)
    shutil.rmtree(os.path.join(base, "doomed"))
    app.send("fm-refresh", str(fm))
    state = wait_applet(app, fm, lambda s: not s["loading"] and s["count"] == 0)
    results.check("a folder deleted while shown empties", state and state["count"] == 0, state)
    app.send("dismiss-presented")
    state = go(shown)
    results.check("File Manager goes on after that", state and "plain.txt" in state["names"], state)

    # MotePad, from a real shell for one and the hook for the name a shell would need quoted.
    before = {w["id"] for w in applet_windows(app.dump(), "motepad")}
    app.send("focus", str([w["id"] for w in app.dump()["windows"] if hosted_info(w, root)][0]))
    app.send("type", "%s ws open motepad %s/plain.txt\\n" % (root, shown))
    dump = wait_for(app, lambda d: any(w["id"] not in before for w in applet_windows(d, "motepad")))
    pad = next((w for w in applet_windows(dump, "motepad") if w["id"] not in before), None)
    results.check("ws open hands a path to MotePad", pad is not None, dump)
    if pad:
        state = wait_applet(app, pad["id"], lambda s: s["length"] > 0)
        results.check("MotePad reads the file", state and state["text"] == "hello\n" and not state["dirty"], state)
        app.send("motepad-insert", "%d more\\n" % pad["id"])
        state = wait_applet(app, pad["id"], lambda s: s["dirty"])
        results.check("typing marks it changed", state and state["dirty"], state)
        app.screenshot("motepad-typed")
        app.send("motepad-save", str(pad["id"]))
        state = wait_applet(app, pad["id"], lambda s: not s["dirty"])
        results.check("Save clears the mark", state and not state["dirty"], state)
        with open(os.path.join(base, "plain.txt")) as handle:
            text = handle.read()
        results.check("Save reaches the disk", "more" in text and "hello" in text, text)
    window = open_file(app, "motepad", "%s/%s" % (shown, UNICODE_NAME))
    state = wait_applet(app, window["id"], lambda s: s["length"] > 0) if window else None
    results.check("MotePad opens a Unicode name", state and state["text"] == "snow ☃\n", state)
    for name in ["latin1.txt", "large.txt"]:
        with open(os.path.join(base, name), "rb") as handle:
            before_bytes = handle.read()
        window = open_file(app, "motepad", "%s/%s" % (shown, name))
        time.sleep(2)
        state = applet_window(app.dump(), window["id"])["applet"] if window else None
        results.check("MotePad refuses %s" % name, state and state["length"] == 0 and not state["dirty"], state)
        app.screenshot("motepad-refuses-%s" % name.split(".")[0])
        app.send("dismiss-presented")
        with open(os.path.join(base, name), "rb") as handle:
            results.check("%s is unchanged" % name, handle.read() == before_bytes)
    window = open_file(app, "motepad", "%s/gone-soon.txt" % shown)
    if window:
        wait_applet(app, window["id"], lambda s: s["length"] > 0)
        os.remove(os.path.join(base, "gone-soon.txt"))
        time.sleep(5)
        state = applet_window(app.dump(), window["id"])["applet"]
        results.check("a file deleted while open keeps its text", state["text"] == "keep me\n", state)
        app.screenshot("motepad-file-gone")
        app.send("dismiss-presented")
    close_tools(app, "motepad")
    wait_for(app, lambda d: not applet_windows(d, "motepad"))
    app.send("dismiss-presented")

    # The viewers.
    window = open_file(app, "markdown", "%s/note.md" % shown)
    state = wait_applet(app, window["id"], lambda s: s["length"] > 0) if window else None
    results.check("Markdown renders a file", state and not state["message"], state)
    app.screenshot("markdown-note")
    window = open_file(app, "markdown", "%s/missing.md" % shown)
    state = wait_applet(app, window["id"], lambda s: s["message"]) if window else None
    results.check("Markdown says a file is missing", state and state["message"], state)
    window = open_file(app, "imageviewer", "%s/pic.png" % shown)
    state = wait_applet(app, window["id"], lambda s: s["hasImage"]) if window else None
    results.check("Image Viewer shows an image", state and state["hasImage"], state)
    app.screenshot("image-viewer")
    for name in ["zero.png", "corrupt.png"]:
        window = open_file(app, "imageviewer", "%s/%s" % (shown, name))
        state = wait_applet(app, window["id"], lambda s: s["message"]) if window else None
        results.check("Image Viewer says %s is unreadable" % name, state and state["message"] and not state["hasImage"], state)
    close_tools(app, "markdown", "imageviewer")

    # Storage over the whole tree, the big folder included.
    close_tools(app, "storage")
    wait_for(app, lambda d: not applet_windows(d, "storage"))
    app.send("open-or-focus", "storage")
    dump = wait_for(app, lambda d: applet_windows(d, "storage") and not applet_windows(d, "storage")[0]["applet"].get("scanning"), 60)
    results.check("Storage finishes its scan", bool(dump), dump)
    app.screenshot("storage-scanned")
    close_tools(app, "storage")

    # What the service must refuse, whatever an applet asks.
    for token, path in [("r-home", home), ("r-root", "/"), ("r-empty", "")]:
        result = file_op(app, token, "remove-tree", path)
        results.check("the service refuses removing %r" % path, result.get("token") == token and not result.get("ok"), result)
    results.check("home is still there", os.path.isdir(app.home))
    result = file_op(app, "l-big", "list", "%s/big" % shown)
    results.check("the service lists a big folder", result.get("count") == 10000, result)
    result = file_op(app, "l-loop", "list", "%s/loop" % shown)
    results.check("a symlink loop is an error, not a hang", result.get("token") == "l-loop" and not result.get("ok"), result)
    app.clear_file("wt_mount_ok")
    app.send("focus", str([w["id"] for w in app.dump()["windows"] if hosted_info(w, root)][0]))
    mounted = app.wait_file("wt_mount_ok", root, "mkdir -p ~/wt_mnt && mount --bind ~/wt_files/mntsrc ~/wt_mnt && echo ok > ~/wt_mount_ok")
    results.check("mount --bind works", mounted == "ok", mounted)
    result = file_op(app, "r-mount", "remove-tree", "%s/wt_mnt" % home)
    results.check("the service refuses removing a mount target", not result.get("ok") and "mount" in result.get("error", ""), result)
    results.check("the mounted folder's own files stay", os.path.exists(os.path.join(base, "mntsrc", "held.txt")))
    result = file_op(app, "l-mount", "list", "%s/wt_mnt" % home)
    results.check("a mount target lists its source", result.get("count") == 1, result)
    app.clear_file("wt_umount_ok")
    text = app.wait_file("wt_umount_ok", root, "umount ~/wt_mnt; echo $? > ~/wt_umount_ok")
    results.check("umount works", text == "0", text)

    # Two windows of one applet and a folder come back, on a launch straight into Workspace,
    # when the runtime may not have set its paths up yet.
    first = open_file(app, "motepad", "%s/plain.txt" % shown)
    second = open_file(app, "motepad", "%s/other.txt" % shown)
    results.check("two MotePad windows open", first is not None and second is not None and first["id"] != second["id"])
    app.send("fm-navigate", "%d %s" % (fm, shown))
    wait_applet(app, fm, lambda s: s["path"] == shown and not s["loading"])
    app.send("save-arrangement")
    wait_for(app, lambda d: d.get("saved"))
    app.send("opens-at-launch", "1")
    app.terminate()
    time.sleep(2)
    app.launch()
    time.sleep(1)
    results.check("hooks answer after the file relaunch", app.wait_ready(), "no ready.json")
    dump = wait_for(app, lambda d: d.get("mode") == "workspace" and len(applet_windows(d, "motepad")) == 2, 30)
    results.check("a launch into Workspace restores both MotePads", bool(dump), app.dump())
    if dump:
        wait_for(app, lambda d: all(w["applet"]["length"] > 0 for w in applet_windows(d, "motepad")))
        paths = sorted(w["applet"]["path"] for w in applet_windows(app.dump(), "motepad"))
        results.check("each keeps its file", paths == ["%s/other.txt" % shown, "%s/plain.txt" % shown], paths)
        window = next(iter(applet_windows(dump, "filemanager")), None)
        results.check("File Manager comes back", window is not None, dump)
        if window:
            state = wait_applet(app, window["id"], lambda s: not s["loading"] and s["path"] == shown)
            results.check("its folder resolves at launch", state and "plain.txt" in state["names"], state)
    app.screenshot("after-file-relaunch")
    app.send("opens-at-launch", "0")


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
        run_file_tests(app, results)
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
