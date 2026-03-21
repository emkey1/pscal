#!/usr/bin/env python3
"""Analyze PSCALI_SSH_DEBUG/Xcode logs for SSH session race symptoms."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Tuple

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
CREATE_RE = re.compile(r"\[ssh-session\] create start session=(\d+)")
THREAD_CREATED_RE = re.compile(r"\[ssh-session\] thread created session=(\d+)")
THREAD_START_RE = re.compile(r"\[ssh-session\] thread start session=(\d+)")
VPROC_CREATED_RE = re.compile(r"\[ssh-session\] vproc created session=(\d+) pid=(\d+)")
THREAD_EXIT_RE = re.compile(r"\[ssh-session\] thread exit session=(\d+) status=(-?\d+)")
OPEN_RESULT_RE = re.compile(r"\[ssh-session\] open result=([^\s]+)")
SSH_EXIT_UI_RE = re.compile(r"SSH exited with status\s+(-?\d+)")
BANNER_RACE_TEXT = "banner exchange: Connection to UNKNOWN port -1: Socket is not connected"


@dataclass
class SessionStats:
    session_id: str
    create_count: int = 0
    thread_created_count: int = 0
    thread_start_count: int = 0
    vproc_created_count: int = 0
    thread_exit_count: int = 0
    exit_statuses: List[int] = field(default_factory=list)
    first_line: int = 0
    last_line: int = 0


@dataclass
class Finding:
    severity: str
    line: int
    message: str


def parse_log(path: Path) -> Tuple[Dict[str, SessionStats], List[Finding], Dict[str, int]]:
    sessions: Dict[str, SessionStats] = {}
    findings: List[Finding] = []
    counters = {
        "open_result_nonzero": 0,
        "open_result_eagain": 0,
        "banner_socket_not_connected": 0,
        "ui_exit_255": 0,
        "ui_exit_nonzero": 0,
        "lines_scanned": 0,
    }

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = ANSI_RE.sub("", raw.rstrip("\n"))
            counters["lines_scanned"] += 1

            if BANNER_RACE_TEXT in line:
                counters["banner_socket_not_connected"] += 1
                findings.append(Finding("error", lineno, BANNER_RACE_TEXT))

            open_match = OPEN_RESULT_RE.search(line)
            if open_match:
                result = open_match.group(1)
                if result not in {"0", "success"}:
                    if result == "EAGAIN":
                        counters["open_result_eagain"] += 1
                    else:
                        counters["open_result_nonzero"] += 1
                        findings.append(
                            Finding("warning", lineno, f"non-zero open result: {result}")
                        )

            ui_exit_match = SSH_EXIT_UI_RE.search(line)
            if ui_exit_match:
                status = int(ui_exit_match.group(1))
                if status == 255:
                    counters["ui_exit_255"] += 1
                    findings.append(Finding("warning", lineno, "UI reported SSH exit status 255"))
                elif status != 0:
                    counters["ui_exit_nonzero"] += 1
                    findings.append(
                        Finding("warning", lineno, f"UI reported non-zero SSH exit status {status}")
                    )

            def ensure_session(sid: str) -> SessionStats:
                st = sessions.get(sid)
                if st is None:
                    st = SessionStats(session_id=sid, first_line=lineno, last_line=lineno)
                    sessions[sid] = st
                st.last_line = lineno
                return st

            create_match = CREATE_RE.search(line)
            if create_match:
                sid = create_match.group(1)
                st = ensure_session(sid)
                st.create_count += 1
                continue

            thread_created_match = THREAD_CREATED_RE.search(line)
            if thread_created_match:
                sid = thread_created_match.group(1)
                st = ensure_session(sid)
                st.thread_created_count += 1
                continue

            thread_start_match = THREAD_START_RE.search(line)
            if thread_start_match:
                sid = thread_start_match.group(1)
                st = ensure_session(sid)
                st.thread_start_count += 1
                continue

            vproc_created_match = VPROC_CREATED_RE.search(line)
            if vproc_created_match:
                sid = vproc_created_match.group(1)
                st = ensure_session(sid)
                st.vproc_created_count += 1
                continue

            thread_exit_match = THREAD_EXIT_RE.search(line)
            if thread_exit_match:
                sid = thread_exit_match.group(1)
                status = int(thread_exit_match.group(2))
                st = ensure_session(sid)
                st.thread_exit_count += 1
                st.exit_statuses.append(status)
                if status != 0:
                    findings.append(
                        Finding("warning", lineno, f"session {sid} exited with status {status}")
                    )
                continue

    for sid, st in sorted(sessions.items(), key=lambda kv: int(kv[0])):
        if st.thread_start_count != st.thread_exit_count:
            findings.append(
                Finding(
                    "warning",
                    st.last_line,
                    (
                        f"session {sid} lifecycle mismatch:"
                        f" starts={st.thread_start_count} exits={st.thread_exit_count}"
                    ),
                )
            )
        if st.create_count == 0 and st.thread_start_count > 0:
            findings.append(
                Finding("warning", st.first_line, f"session {sid} has thread start with no create log")
            )
        if st.thread_created_count == 0 and st.thread_start_count > 0:
            findings.append(
                Finding(
                    "warning",
                    st.first_line,
                    f"session {sid} has thread start with no 'thread created' log",
                )
            )

    return sessions, findings, counters


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze PSCALI_SSH_DEBUG logs for SSH tab race symptoms."
    )
    parser.add_argument("log", help="Path to Xcode/device log file")
    parser.add_argument("--json", action="store_true", help="Emit JSON report")
    parser.add_argument(
        "--fail-on-warning",
        action="store_true",
        help="Return non-zero when warnings are present",
    )
    args = parser.parse_args()

    log_path = Path(args.log)
    if not log_path.is_file():
        print(f"error: log file not found: {log_path}", file=sys.stderr)
        return 2

    sessions, findings, counters = parse_log(log_path)

    errors = [f for f in findings if f.severity == "error"]
    warnings = [f for f in findings if f.severity == "warning"]

    if args.json:
        payload = {
            "file": str(log_path),
            "counters": counters,
            "sessions": {
                sid: {
                    "create_count": st.create_count,
                    "thread_created_count": st.thread_created_count,
                    "thread_start_count": st.thread_start_count,
                    "vproc_created_count": st.vproc_created_count,
                    "thread_exit_count": st.thread_exit_count,
                    "exit_statuses": st.exit_statuses,
                    "first_line": st.first_line,
                    "last_line": st.last_line,
                }
                for sid, st in sorted(sessions.items(), key=lambda kv: int(kv[0]))
            },
            "findings": [
                {"severity": f.severity, "line": f.line, "message": f.message} for f in findings
            ],
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print(f"SSH debug log analysis: {log_path}")
        print(f"lines scanned: {counters['lines_scanned']}")
        print(f"sessions seen: {len(sessions)}")
        print(
            "signals:"
            f" banner_socket_not_connected={counters['banner_socket_not_connected']},"
            f" ui_exit_255={counters['ui_exit_255']},"
            f" ui_exit_nonzero={counters['ui_exit_nonzero']},"
            f" open_result_nonzero={counters['open_result_nonzero']},"
            f" open_result_eagain={counters['open_result_eagain']}"
        )

        if sessions:
            print("\nsession summary:")
            for sid, st in sorted(sessions.items(), key=lambda kv: int(kv[0])):
                statuses = ",".join(str(s) for s in st.exit_statuses) if st.exit_statuses else "-"
                print(
                    f"  session {sid}: create={st.create_count}, created={st.thread_created_count},"
                    f" start={st.thread_start_count}, vproc={st.vproc_created_count},"
                    f" exit={st.thread_exit_count}, statuses={statuses},"
                    f" lines={st.first_line}-{st.last_line}"
                )

        if findings:
            print("\nfindings:")
            for f in findings:
                print(f"  [{f.severity}] line {f.line}: {f.message}")
        else:
            print("\nfindings: none")

    if errors:
        return 1
    if warnings and args.fail_on_warning:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
