#!/usr/bin/env python3
"""
Validate iOS/iPadOS resize propagation from real Xcode console logs.

This harness is log-driven by design: it checks the runtime chain that the
host-only C harness cannot exercise directly.
"""

from __future__ import annotations

import argparse
import dataclasses
import re
import sys
from collections import defaultdict, deque
from typing import Deque, Dict, List, Optional, Tuple


RE_BIND = re.compile(
    r"\[ssh-resize\] hterm\[(?P<hterm>\d+)\] bind-session previous=(?P<prev>\d+) session=(?P<session>\d+)"
)
RE_NATIVE = re.compile(
    r"\[ssh-resize\] hterm\[(?P<hterm>\d+)\] native-resize=(?P<cols>\d+)x(?P<rows>\d+)"
)
RE_FORWARD = re.compile(
    r"\[ssh-resize\] hterm\[(?P<hterm>\d+)\] runtime-forward source=(?P<source>\S+) session=(?P<session>\d+) cols=(?P<cols>\d+) rows=(?P<rows>\d+)"
)
RE_DEFER = re.compile(
    r"\[ssh-resize\] hterm\[(?P<hterm>\d+)\] runtime-defer source=(?P<source>\S+) session=(?P<session>\d+) cols=(?P<cols>\d+) rows=(?P<rows>\d+)"
)
RE_UPDATE = re.compile(
    r"\[micro-resize\] runtime updateSessionWindowSize session=(?P<session>\d+) cols=(?P<cols>\d+) rows=(?P<rows>\d+)"
)
RE_VPROC = re.compile(
    r"\[micro-resize\] vproc setSessionWinsize (?P<kind>applied|missing-pty) session=(?P<session>\d+) cols=(?P<cols>\d+) rows=(?P<rows>\d+)"
)


@dataclasses.dataclass
class ForwardEvent:
    line: int
    hterm: int
    session: int
    cols: int
    rows: int
    matched_update_line: Optional[int] = None


@dataclasses.dataclass
class UpdateEvent:
    line: int
    session: int
    cols: int
    rows: int
    from_forward_line: Optional[int] = None
    vproc_line: Optional[int] = None
    vproc_kind: Optional[str] = None
    missing_resolved_line: Optional[int] = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate iOS resize propagation from Xcode logs")
    parser.add_argument("logfile", help="Path to captured Xcode console output")
    parser.add_argument(
        "--max-forward-gap",
        type=int,
        default=250,
        help="Maximum line distance from runtime-forward to runtime update (default: 250)",
    )
    parser.add_argument(
        "--max-update-gap",
        type=int,
        default=250,
        help="Maximum line distance from runtime update to vproc result (default: 250)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        with open(args.logfile, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError as exc:
        print(f"error: failed to read log file: {exc}", file=sys.stderr)
        return 2

    bound_session_by_hterm: Dict[int, int] = defaultdict(int)
    observed_sizes_by_session: Dict[int, set[Tuple[int, int]]] = defaultdict(set)

    forwards: List[ForwardEvent] = []
    updates: List[UpdateEvent] = []
    pending_forwards: Deque[ForwardEvent] = deque()
    pending_updates: Deque[UpdateEvent] = deque()
    pending_missing: Dict[Tuple[int, int], Deque[UpdateEvent]] = defaultdict(deque)

    native_while_bound = 0
    defer_while_bound_failures: List[str] = []
    failures: List[str] = []

    for line_no, raw in enumerate(lines, start=1):
        line = raw.rstrip("\n")

        m = RE_BIND.search(line)
        if m:
            hterm = int(m.group("hterm"))
            session = int(m.group("session"))
            bound_session_by_hterm[hterm] = session
            continue

        m = RE_NATIVE.search(line)
        if m:
            hterm = int(m.group("hterm"))
            cols = int(m.group("cols"))
            rows = int(m.group("rows"))
            session = bound_session_by_hterm.get(hterm, 0)
            if session != 0:
                native_while_bound += 1
                observed_sizes_by_session[session].add((cols, rows))
            continue

        m = RE_FORWARD.search(line)
        if m:
            evt = ForwardEvent(
                line=line_no,
                hterm=int(m.group("hterm")),
                session=int(m.group("session")),
                cols=int(m.group("cols")),
                rows=int(m.group("rows")),
            )
            forwards.append(evt)
            pending_forwards.append(evt)
            observed_sizes_by_session[evt.session].add((evt.cols, evt.rows))
            continue

        m = RE_DEFER.search(line)
        if m:
            hterm = int(m.group("hterm"))
            bound_session = bound_session_by_hterm.get(hterm, 0)
            defer_session = int(m.group("session"))
            if bound_session != 0 or defer_session != 0:
                defer_while_bound_failures.append(
                    f"line {line_no}: runtime-defer while bound hterm={hterm} bound={bound_session} defer-session={defer_session}"
                )
            continue

        m = RE_UPDATE.search(line)
        if m:
            update = UpdateEvent(
                line=line_no,
                session=int(m.group("session")),
                cols=int(m.group("cols")),
                rows=int(m.group("rows")),
            )
            updates.append(update)

            # Match to earliest pending forward with same size+session.
            matched_forward: Optional[ForwardEvent] = None
            for candidate in pending_forwards:
                if (
                    candidate.session == update.session
                    and candidate.cols == update.cols
                    and candidate.rows == update.rows
                    and (line_no - candidate.line) <= args.max_forward_gap
                ):
                    matched_forward = candidate
                    break
            if matched_forward:
                matched_forward.matched_update_line = line_no
                update.from_forward_line = matched_forward.line
                pending_forwards.remove(matched_forward)
            pending_updates.append(update)
            continue

        m = RE_VPROC.search(line)
        if m:
            kind = m.group("kind")
            session = int(m.group("session"))
            cols = int(m.group("cols"))
            rows = int(m.group("rows"))

            matched_update: Optional[UpdateEvent] = None
            for candidate in pending_updates:
                if (
                    candidate.session == session
                    and candidate.cols == cols
                    and candidate.rows == rows
                    and (line_no - candidate.line) <= args.max_update_gap
                    and candidate.vproc_line is None
                ):
                    matched_update = candidate
                    break
            if not matched_update:
                continue

            matched_update.vproc_line = line_no
            matched_update.vproc_kind = kind
            pending_updates.remove(matched_update)

            if kind == "missing-pty":
                pending_missing[(session, cols, rows)].append(matched_update)
            else:
                queue = pending_missing.get((session, cols, rows))
                if queue:
                    prior = queue.popleft()
                    prior.missing_resolved_line = line_no
            continue

    for fwd in pending_forwards:
        failures.append(
            f"line {fwd.line}: runtime-forward session={fwd.session} size={fwd.cols}x{fwd.rows} had no matching runtime update"
        )

    for upd in pending_updates:
        failures.append(
            f"line {upd.line}: runtime update session={upd.session} size={upd.cols}x{upd.rows} had no matching vproc result"
        )

    for key, queue in pending_missing.items():
        session, cols, rows = key
        while queue:
            upd = queue.popleft()
            failures.append(
                f"line {upd.vproc_line}: missing-pty never resolved session={session} size={cols}x{rows}"
            )

    failures.extend(defer_while_bound_failures)

    print("Resize Log Harness Report")
    print(f"- lines: {len(lines)}")
    print(f"- runtime-forward events: {len(forwards)}")
    print(f"- runtime update events: {len(updates)}")
    print(f"- native-resize while bound: {native_while_bound}")
    dynamic_sessions = {
        session: sizes for session, sizes in observed_sizes_by_session.items() if len(sizes) > 1 and session != 0
    }
    print(f"- sessions with dynamic size changes: {len(dynamic_sessions)}")
    for session in sorted(dynamic_sessions):
        sizes = sorted(dynamic_sessions[session])
        preview = ", ".join(f"{c}x{r}" for c, r in sizes[:6])
        suffix = "" if len(sizes) <= 6 else ", ..."
        print(f"  - session {session}: {preview}{suffix}")

    if failures:
        print("FAIL")
        for item in failures[:50]:
            print(f"- {item}")
        if len(failures) > 50:
            print(f"- ... and {len(failures) - 50} more")
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

