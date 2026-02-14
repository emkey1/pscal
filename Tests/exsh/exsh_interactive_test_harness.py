#!/usr/bin/env python3
"""PTY-driven interactive regression harness for exsh signal behavior."""

from __future__ import annotations

import argparse
import os
import pty
import re
import selectors
import signal
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional

HARNESS_ROOT = Path(__file__).resolve().parent
REPO_ROOT = HARNESS_ROOT.parent.parent
DEFAULT_EXE = REPO_ROOT / "build" / "ios-host" / "bin" / "exsh"
DEFAULT_TIMEOUT = 2.0
MARKER_DIR = REPO_ROOT / ".tmp_tests" / "exsh_interactive"

ANSI_CSI_RE = re.compile(r"\x1B\[[0-9;?]*[ -/]*[@-~]")
ANSI_OSC_RE = re.compile(r"\x1B\][^\x07]*(?:\x07|\x1b\\)")
LPS_WATCH_RE = re.compile(
    r"^\s*(\d+)\s+\d+\s+\w+\s+[0-9]+(?:\.[0-9]+)?\s+[0-9]+(?:\.[0-9]+)?\s+watch -n 1 date\s*$",
    re.MULTILINE,
)


@dataclass
class Scenario:
    test_id: str
    name: str
    run: Callable[["PtyShell"], tuple[bool, str]]


@dataclass
class Result:
    scenario: Scenario
    passed: bool
    reason: str
    output_tail: str


class PtyShell:
    def __init__(self, executable: Path, timeout: float):
        self.executable = executable
        self.timeout = timeout
        self.master_fd: Optional[int] = None
        self.proc: Optional[subprocess.Popen[bytes]] = None
        self.selector: Optional[selectors.BaseSelector] = None
        self.output = ""
        self._spawn()

    def _spawn(self) -> None:
        master_fd, slave_fd = pty.openpty()
        env = os.environ.copy()
        env.setdefault("TERM", "xterm-256color")
        env["EXSH_SKIP_RC"] = "1"
        env["PS1"] = "PROMPT> "
        self.proc = subprocess.Popen(
            [str(self.executable)],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            cwd=str(REPO_ROOT),
            env=env,
            close_fds=True,
            start_new_session=True,
        )
        os.close(slave_fd)
        os.set_blocking(master_fd, False)
        selector = selectors.DefaultSelector()
        selector.register(master_fd, selectors.EVENT_READ)
        self.master_fd = master_fd
        self.selector = selector

    def close(self) -> None:
        if self.selector is not None:
            self.selector.close()
            self.selector = None
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass
            try:
                self.proc.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except OSError:
                    pass
                self.proc.wait(timeout=0.5)
        self.proc = None
        if self.master_fd is not None:
            try:
                os.close(self.master_fd)
            except OSError:
                pass
            self.master_fd = None

    def _normalise(self, data: bytes) -> str:
        text = data.decode("utf-8", errors="replace")
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        text = ANSI_CSI_RE.sub("", text)
        text = ANSI_OSC_RE.sub("", text)
        return text

    def _pump(self, duration: float) -> None:
        if self.selector is None or self.master_fd is None:
            return
        end = time.monotonic() + duration
        while time.monotonic() < end:
            events = self.selector.select(timeout=0.05)
            for key, _ in events:
                try:
                    chunk = os.read(key.fd, 8192)
                except OSError:
                    chunk = b""
                if chunk:
                    self.output += self._normalise(chunk)

    def wait_for_substring(self, needle: str, timeout: Optional[float] = None) -> bool:
        if timeout is None:
            timeout = self.timeout
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._pump(0.05)
            if needle in self.output:
                return True
        return needle in self.output

    def send(self, data: bytes) -> None:
        if self.master_fd is None:
            raise RuntimeError("PTY closed")
        os.write(self.master_fd, data)

    def send_line(self, line: str) -> None:
        self.send(line.encode("utf-8") + b"\n")

    def wait_for_path(self, path: Path, timeout: Optional[float] = None) -> bool:
        if timeout is None:
            timeout = self.timeout
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._pump(0.05)
            if path.exists():
                return True
        return path.exists()

    def tail(self, max_chars: int = 1800) -> str:
        return self.output[-max_chars:]


def _new_marker_path(test_id: str) -> Path:
    stamp = int(time.time() * 1000)
    MARKER_DIR.mkdir(parents=True, exist_ok=True)
    return MARKER_DIR / f"{test_id}-{os.getpid()}-{stamp}.txt"


def _unlink_if_exists(path: Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def _shell_path(path: Path) -> str:
    try:
        rel = path.relative_to(REPO_ROOT)
        return f"./{rel.as_posix()}"
    except ValueError:
        return path.as_posix()


def _send_until_marker(shell: PtyShell, command: str, marker: Path, attempts: int, timeout: float) -> bool:
    for _ in range(max(1, attempts)):
        shell.send_line(command)
        if shell.wait_for_path(marker, timeout=timeout):
            return True
        shell._pump(0.15)
    return marker.exists()


def _wait_for_prompt(shell: PtyShell) -> tuple[bool, str]:
    if shell.wait_for_substring("PROMPT> ", timeout=2.0):
        return True, "prompt ready"
    return False, "interactive prompt did not appear"


def _prompt_count(shell: PtyShell) -> int:
    return shell.output.count("PROMPT> ")


def _latest_watch_pid(text: str) -> Optional[int]:
    matches = list(LPS_WATCH_RE.finditer(text))
    if not matches:
        return None
    return int(matches[-1].group(1))


def scenario_ctrl_c_interrupts_sleep(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-sleep")
    _unlink_if_exists(marker)
    shell.send_line("sleep 10")
    shell._pump(0.25)
    shell.send(b"\x03")
    shell._pump(0.20)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.5):
        return False, "Ctrl-C did not return control to shell during sleep"
    _unlink_if_exists(marker)
    return True, "sleep interrupted by Ctrl-C"


def scenario_ctrl_z_stops_sleep(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlz-jobs")
    _unlink_if_exists(marker)
    shell.send_line("sleep 10")
    shell._pump(0.25)
    shell.send(b"\x1a")
    shell._pump(0.25)
    shell.send_line(f"jobs | grep Stopped | grep 'sleep 10' && touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.5):
        return False, "Ctrl-Z did not return control to shell for jobs query"
    _unlink_if_exists(marker)
    return True, "sleep stopped and visible in jobs output"


def scenario_ctrl_z_stops_local_vproc_and_restores_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker_stop = _new_marker_path("ctrlz-watch-stop")
    marker_bg = _new_marker_path("ctrlz-watch-bg")
    marker_fg = _new_marker_path("ctrlz-watch-fg")
    pid_before_path = _new_marker_path("ctrlz-watch-pid-before")
    pid_after_path = _new_marker_path("ctrlz-watch-pid-after")
    _unlink_if_exists(marker_stop)
    _unlink_if_exists(marker_bg)
    _unlink_if_exists(marker_fg)
    _unlink_if_exists(pid_before_path)
    _unlink_if_exists(pid_after_path)
    prompt_before = _prompt_count(shell)
    shell.send_line("watch -n 1 date")
    shell._pump(0.90)
    shell.send(b"\x1a")
    shell._pump(1.30)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return shell prompt after suspending foreground vproc"
    shell.send_line(
        "jobs | grep Stopped | grep 'watch -n' | grep date >/dev/null"
        " && lps | grep 'watch -n' >/dev/null"
        f" && touch {_shell_path(marker_stop)}"
    )
    if not shell.wait_for_path(marker_stop, timeout=3.0):
        return False, "Suspended local vproc was not queryable from shell after Ctrl-Z"
    shell.send_line(
        f"lps | grep 'watch -n 1 date' > {_shell_path(pid_before_path)}"
    )
    if not shell.wait_for_path(pid_before_path, timeout=3.0):
        return False, "Unable to capture lps output for stopped watch"
    stopped_pid_before = _latest_watch_pid(pid_before_path.read_text(errors="replace"))
    if stopped_pid_before is None:
        return False, "Unable to capture stopped watch pid after first Ctrl-Z"

    prompt_after_stop = _prompt_count(shell)
    shell.send_line(
        "bg %1 >/dev/null 2>&1"
        " && jobs | grep Running | grep 'watch -n' | grep date >/dev/null"
        f" && touch {_shell_path(marker_bg)}"
    )
    if not shell.wait_for_path(marker_bg, timeout=3.0):
        return False, "bg did not resume suspended local vproc as Running"
    if _prompt_count(shell) <= prompt_after_stop:
        return False, "Shell prompt did not remain responsive after bg"

    shell.send_line("fg %1")
    shell._pump(0.90)
    shell.send(b"\x1a")
    shell._pump(1.10)
    shell.send_line(
        "jobs | grep Stopped | grep 'watch -n' | grep date >/dev/null"
        f" && touch {_shell_path(marker_fg)}"
    )
    if not shell.wait_for_path(marker_fg, timeout=3.0):
        return False, "watch was not stopped again after fg -> Ctrl-Z"

    shell.send_line(
        f"lps | grep 'watch -n 1 date' > {_shell_path(pid_after_path)}"
    )
    if not shell.wait_for_path(pid_after_path, timeout=3.0):
        return False, "Unable to capture lps output after fg -> Ctrl-Z"
    stopped_pid_after = _latest_watch_pid(pid_after_path.read_text(errors="replace"))
    if stopped_pid_after is None:
        return False, "Unable to capture stopped watch pid after fg -> Ctrl-Z"
    if stopped_pid_after != stopped_pid_before:
        return False, "fg restarted watch instead of resuming existing stopped task"

    shell.send_line("kill %1 >/dev/null 2>&1 || true")
    shell._pump(0.2)

    _unlink_if_exists(marker_stop)
    _unlink_if_exists(marker_bg)
    _unlink_if_exists(marker_fg)
    _unlink_if_exists(pid_before_path)
    _unlink_if_exists(pid_after_path)
    return True, "Ctrl-Z/bg/fg flow works for shell-launched local vproc"


def scenario_ctrl_z_stops_clike_frontend_and_restores_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker_stop = _new_marker_path("ctrlz-clike-stop")
    _unlink_if_exists(marker_stop)
    prompt_before = _prompt_count(shell)
    shell.send_line("PSCALI_WORDS_PATH=etc/words clike Examples/clike/base/hangman5")
    shell._pump(1.00)
    shell.send(b"\x1a")
    shell._pump(1.20)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return shell prompt after suspending clike frontend"
    check_cmd = (
        "lps | grep 'hangman5' >/dev/null"
        f" && touch {_shell_path(marker_stop)}"
    )
    if not _send_until_marker(shell, check_cmd, marker_stop, attempts=3, timeout=4.0):
        return False, "Suspended clike frontend was not queryable from shell after Ctrl-Z"
    _unlink_if_exists(marker_stop)
    return True, "Ctrl-Z stops clike frontend and restores shell prompt"


def scenario_ctrl_c_interrupts_watch(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-watch")
    _unlink_if_exists(marker)
    shell.send_line("watch -n 1 date")
    shell._pump(0.90)
    shell.send(b"\x03")
    shell._pump(0.25)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.8):
        return False, "Ctrl-C did not return control to shell during watch"
    _unlink_if_exists(marker)
    return True, "watch interrupted by Ctrl-C"


def scenario_ctrl_c_at_prompt_keeps_shell_alive(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-prompt")
    _unlink_if_exists(marker)
    shell.send(b"\x03")
    shell._pump(0.15)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.0):
        return False, "Ctrl-C at prompt left shell unresponsive"
    _unlink_if_exists(marker)
    return True, "prompt-level Ctrl-C preserved shell responsiveness"


SCENARIOS: List[Scenario] = [
    Scenario(
        test_id="interactive_ctrl_c_prompt",
        name="Ctrl-C at prompt keeps shell responsive",
        run=scenario_ctrl_c_at_prompt_keeps_shell_alive,
    ),
    Scenario(
        test_id="interactive_ctrl_c_sleep",
        name="Ctrl-C interrupts foreground sleep",
        run=scenario_ctrl_c_interrupts_sleep,
    ),
    Scenario(
        test_id="interactive_ctrl_z_sleep",
        name="Ctrl-Z stops foreground sleep and reports in jobs",
        run=scenario_ctrl_z_stops_sleep,
    ),
    Scenario(
        test_id="interactive_ctrl_z_local_vproc_prompt",
        name="Ctrl-Z stops local vproc and restores shell prompt",
        run=scenario_ctrl_z_stops_local_vproc_and_restores_prompt,
    ),
    Scenario(
        test_id="interactive_ctrl_z_clike_frontend_prompt",
        name="Ctrl-Z stops clike frontend and restores shell prompt",
        run=scenario_ctrl_z_stops_clike_frontend_and_restores_prompt,
    ),
    Scenario(
        test_id="interactive_ctrl_c_watch",
        name="Ctrl-C interrupts foreground watch",
        run=scenario_ctrl_c_interrupts_watch,
    ),
]


def ensure_executable(path_override: Optional[str]) -> Path:
    exe = Path(path_override).expanduser() if path_override else DEFAULT_EXE
    if not exe.exists():
        raise FileNotFoundError(f"exsh executable not found at {exe}")
    return exe


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run interactive PTY exsh signal regressions")
    parser.add_argument("--executable", type=str, default=None, help="Override exsh executable path")
    parser.add_argument("--only", type=str, default=None, help="Run only tests whose id contains substring")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="Per-step timeout in seconds")
    parser.add_argument("--list", action="store_true", help="List tests and exit")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    scenarios = SCENARIOS
    if args.only:
        scenarios = [s for s in scenarios if args.only in s.test_id]
    if args.list:
        for s in scenarios:
            print(f"{s.test_id}: {s.name}")
        return 0
    if not scenarios:
        print("No interactive tests selected")
        return 1

    try:
        exe = ensure_executable(args.executable)
    except FileNotFoundError as exc:
        print(str(exc))
        return 1

    results: List[Result] = []
    for scenario in scenarios:
        shell = PtyShell(executable=exe, timeout=args.timeout)
        try:
            passed, reason = scenario.run(shell)
            results.append(
                Result(
                    scenario=scenario,
                    passed=passed,
                    reason=reason,
                    output_tail=shell.tail(),
                )
            )
        finally:
            shell.close()

    failures = [r for r in results if not r.passed]
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        print(f"[{status}] {result.scenario.test_id} - {result.scenario.name}")
        if not result.passed:
            print(f"    Reason: {result.reason}")
            if result.output_tail.strip():
                print("    Output tail:")
                for line in result.output_tail.splitlines()[-30:]:
                    print(f"        {line}")

    print()
    print(f"Ran {len(results)} interactive test(s); {len(failures)} failure(s)")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
