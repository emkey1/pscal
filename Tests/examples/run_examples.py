#!/usr/bin/env python3
import argparse
import json
import os
import pty
import re
import selectors
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TIMEOUT = 20.0
STARTUP_TIMEOUT = 3.0
ANSI_ESCAPE_RE = re.compile(r"\x1B\[[0-9;?]*[ -/]*[@-~]")
ANSI_OSC_RE = re.compile(r"\x1B\][^\x07]*(?:\x07|\x1b\\)")

INTERPRETERS = {
    "pascal": {
        "binary": REPO_ROOT / "build/bin/pascal",
        "args": ["--no-cache"],
    },
    "clike": {
        "binary": REPO_ROOT / "build/bin/clike",
        "args": ["--no-cache"],
    },
    "rea": {
        "binary": REPO_ROOT / "build/bin/rea",
        "args": ["--no-cache"],
    },
}


class Harness:
    def __init__(self):
        self.total = 0
        self.failures = 0
        self.skips = 0

    def report(self, status: str, test_id: str, description: str, details=None):
        self.total += 1
        status = status.upper()
        if status == "FAIL":
            self.failures += 1
        elif status == "SKIP":
            self.skips += 1
        print(f"[{status}] {test_id} – {description}")
        if details:
            for detail in details:
                if not detail:
                    continue
                for line in detail.splitlines() or (detail,):
                    print(f"    {line}")

    def summary(self, label: str):
        base = f"\nRan {self.total} {label} test(s); {self.failures} failure(s)"
        if self.skips:
            base += f"; {self.skips} skipped"
        print(base)

    def exit_code(self) -> int:
        return 0 if self.failures == 0 else 1


def strip_ansi(data: str) -> str:
    data = ANSI_ESCAPE_RE.sub("", data)
    data = ANSI_OSC_RE.sub("", data)
    return data


def normalise_output(raw: bytes) -> str:
    text = raw.decode("utf-8", errors="replace")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return strip_ansi(text)


def load_manifest(path: Path):
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def build_command(entry, script_path):
    language = entry["language"]
    interp = INTERPRETERS.get(language)
    if not interp:
        raise ValueError(f"Unsupported language: {language}")
    binary = interp["binary"]
    args = list(interp["args"])
    cmd = [str(binary)] + args + [str(script_path)]
    return binary, cmd


def run_entry(entry, *, timeout: float, startup_only: bool):
    stdin_data = entry.get("stdin", "")
    input_delay = float(entry.get("input_delay", 0.0))
    env_overrides = entry.get("env", {})
    script_path = (REPO_ROOT / entry["path"]).resolve()
    workdir = script_path.parent
    env = os.environ.copy()
    env.setdefault("TERM", "xterm-256color")
    for key, value in env_overrides.items():
        env[str(key)] = str(value)

    if not script_path.exists():
        return {
            "status": "FAIL",
            "details": [f"Example source not found at {script_path}"],
            "stdout": b"",
            "returncode": None,
        }

    binary, cmd = build_command(entry, script_path)
    if not binary.exists():
        return {
            "status": "FAIL",
            "details": [f"Interpreter not found at {binary}"],
            "stdout": b"",
            "returncode": None,
        }

    master_fd, slave_fd = pty.openpty()
    try:
        proc = subprocess.Popen(
            cmd,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            cwd=workdir,
            env=env,
            close_fds=True,
        )
    except Exception as exc:  # pragma: no cover - defensive
        os.close(master_fd)
        os.close(slave_fd)
        return {
            "status": "FAIL",
            "details": [f"Failed to spawn process: {exc}"],
            "stdout": b"",
            "returncode": None,
        }
    finally:
        os.close(slave_fd)

    os.set_blocking(master_fd, False)
    selector = selectors.DefaultSelector()
    selector.register(master_fd, selectors.EVENT_READ)

    stdout_chunks = []
    start_time = time.monotonic()
    deadline = start_time + timeout
    stdin_sent = not stdin_data
    timed_out = False
    input_bytes = stdin_data.encode("utf-8") if stdin_data else b""

    try:
        while True:
            now = time.monotonic()
            if not stdin_sent and now - start_time >= input_delay:
                try:
                    os.write(master_fd, input_bytes)
                except OSError:
                    pass
                stdin_sent = True

            wait_timeout = max(0.0, min(0.1, deadline - now))
            events = selector.select(wait_timeout)
            for key, _ in events:
                try:
                    chunk = os.read(key.fd, 8192)
                except OSError:
                    chunk = b""
                if chunk:
                    stdout_chunks.append(chunk)
                else:
                    selector.unregister(key.fd)

            if proc.poll() is not None:
                break

            if now >= deadline:
                if startup_only:
                    break
                timed_out = True
                break
    finally:
        selector.close()

    if proc.poll() is None:
        if startup_only:
            proc.terminate()
            try:
                proc.wait(1.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        else:
            timed_out = True
            proc.terminate()
            try:
                proc.wait(1.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    # Drain remaining output
    while True:
        try:
            chunk = os.read(master_fd, 8192)
        except OSError:
            break
        if not chunk:
            break
        stdout_chunks.append(chunk)

    os.close(master_fd)

    stdout_data = b"".join(stdout_chunks)
    return {
        "status": "TIMEOUT" if timed_out else "OK",
        "stdout": stdout_data,
        "returncode": proc.returncode,
        "details": [],
    }


def apply_checks(entry, stdout_text: str):
    issues = []
    checks = entry.get("checks", [])
    for check in checks:
        ctype = check.get("type", "contains")
        stream = check.get("stream", "stdout")
        if stream != "stdout":
            issues.append(f"Unsupported stream '{stream}' for check")
            continue
        target = stdout_text
        value = check.get("value", "")
        if ctype == "contains":
            if value not in target:
                issues.append(f"stdout missing substring: {value}")
        elif ctype == "equals":
            if target.strip() != value.strip():
                issues.append("stdout did not match expected text")
        elif ctype == "startswith":
            if not target.startswith(value):
                issues.append(f"stdout does not start with: {value}")
        elif ctype == "endswith":
            if not target.rstrip().endswith(value):
                issues.append(f"stdout does not end with: {value}")
        elif ctype == "nonempty":
            if not target.strip():
                issues.append("stdout was empty")
        else:
            issues.append(f"Unknown check type: {ctype}")
    return issues


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run example program tests")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("examples_manifest.json"),
        help="Path to examples manifest",
    )
    args = parser.parse_args(argv)

    try:
        manifest = load_manifest(args.manifest)
    except Exception as exc:  # pragma: no cover - defensive
        print(f"Failed to load manifest: {exc}", file=sys.stderr)
        return 1

    harness = Harness()

    for entry in manifest:
        test_id = entry.get("id", entry.get("path"))
        description = entry.get("description", entry.get("path"))

        skip_info = entry.get("skip")
        if skip_info:
            env_name = skip_info.get("env")
            expected = skip_info.get("value")
            if env_name and expected is not None:
                if os.environ.get(env_name) != str(expected):
                    reason = skip_info.get("reason", f"Requires {env_name}={expected}")
                    harness.report("SKIP", test_id, description, [reason])
                    continue

        timeout = float(entry.get("timeout", DEFAULT_TIMEOUT))
        startup_only = bool(entry.get("startup_only", False))
        if startup_only:
            timeout = float(entry.get("startup_timeout", STARTUP_TIMEOUT))

        result = run_entry(entry, timeout=timeout, startup_only=startup_only)
        stdout_text = normalise_output(result.get("stdout", b""))

        details = []
        details.extend(result.get("details", []))
        if result.get("status") == "TIMEOUT":
            details.append(f"Process timed out after {timeout:.1f}s")
        issues = apply_checks(entry, stdout_text)

        allowed_codes = entry.get("allow_exit_codes")
        if allowed_codes is None:
            allowed_codes = [0]
        else:
            allowed_codes = [int(code) for code in allowed_codes]

        returncode = result.get("returncode")
        exit_ok = startup_only or (returncode in allowed_codes)
        if not exit_ok:
            details.append(f"Unexpected exit code: {returncode}")

        if issues:
            details.extend(issues)
        if details:
            snippet = stdout_text.strip()
            if snippet:
                if len(snippet) > 500:
                    snippet = snippet[:500] + "…"
                details.append("Captured stdout:\n" + snippet)

        if details:
            harness.report("FAIL", test_id, description, details)
        else:
            harness.report("PASS", test_id, description)

    harness.summary("Example")
    return harness.exit_code()


if __name__ == "__main__":
    sys.exit(main())
