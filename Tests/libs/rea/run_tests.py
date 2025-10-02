#!/usr/bin/env python3
"""Run the opt-in Rea library test suite."""

from __future__ import annotations

import http.server
import os
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


def find_repo_root(script_path: Path) -> Path:
    """Return the repository root based on this script's location."""
    # Tests/libs/rea/run_tests.py -> repo root is three levels up from "rea".
    return script_path.resolve().parents[3]


def _discover_ext_builtins(executable: Path) -> set[str]:
    """Return the set of extended builtin categories exposed by *executable*."""

    try:
        proc = subprocess.run(
            [str(executable), "--dump-ext-builtins"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return set()

    available: set[str] = set()
    for line in proc.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[0] == "category":
            available.add(parts[1])
    return available


def build_env(
    root: Path,
    tmp_dir: Path,
    home_dir: Path,
    base_url: str,
    available_builtins: set[str],
) -> dict[str, str]:
    """Construct the environment for running the Rea test program."""
    env = os.environ.copy()

    lib_dir = root / "lib" / "rea"
    existing_import = env.get("REA_IMPORT_PATH")
    if existing_import:
        env["REA_IMPORT_PATH"] = f"{lib_dir}{os.pathsep}{existing_import}"
    else:
        env["REA_IMPORT_PATH"] = str(lib_dir)

    env["REA_TEST_TMPDIR"] = str(tmp_dir)
    env["REA_TEST_HTTP_BASE_URL"] = base_url
    env["HOME"] = str(home_dir)

    if available_builtins:
        env["REA_TEST_EXT_BUILTINS"] = ",".join(sorted(available_builtins))
        env["REA_TEST_HAS_YYJSON"] = "1" if "yyjson" in available_builtins else "0"

    return env


class _RequestHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args) -> None:  # noqa: A003 - match BaseHTTPRequestHandler signature
        # Silence the default logging to keep the test output tidy.
        return

    def _read_body(self) -> str:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return ""
        data = self.rfile.read(length)
        return data.decode("utf-8", errors="replace")

    def _send_text(self, status: int, body: str, content_type: str = "text/plain") -> None:
        payload = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        if self.path == "/text":
            self._send_text(200, "hello world")
            return
        if self.path == "/json":
            accept = self.headers.get("Accept", "")
            body = f"accept: {accept}\n"
            self._send_text(200, body)
            return
        if self.path == "/download":
            self._send_text(200, "download body")
            return
        if self.path == "/status/404":
            self._send_text(404, "not found")
            return
        self._send_text(404, "unknown path")

    def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        body = self._read_body()
        summary = self._summarize_request("POST", body)
        status = 200
        if self.path == "/post-json":
            status = 200
        self._send_text(status, summary)

    def do_PUT(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        body = self._read_body()
        summary = self._summarize_request("PUT", body)
        self._send_text(200, summary)

    def _summarize_request(self, method: str, body: str) -> str:
        content_type = self.headers.get("Content-Type", "")
        accept = self.headers.get("Accept", "")
        lines = [
            f"method: {method}",
            f"content-type: {content_type}",
            f"accept: {accept}",
            f"body: {body}",
        ]
        return "\n".join(lines)


def start_server() -> tuple[socketserver.TCPServer, str]:
    """Start the local HTTP server for exercising the Http module."""
    # Bind explicitly to 127.0.0.1 so the tests always use IPv4.
    server = socketserver.TCPServer(("127.0.0.1", 0), _RequestHandler, bind_and_activate=False)
    # Avoid "Address already in use" when rerunning quickly.
    server.allow_reuse_address = True
    server.server_bind()
    server.server_activate()
    host, port = server.server_address
    base_url = f"http://{host}:{port}"
    thread = threading.Thread(target=server.serve_forever, name="rea-http-server", daemon=True)
    thread.start()
    return server, base_url


def stop_server(server: socketserver.TCPServer) -> None:
    """Shut down the local HTTP server."""
    try:
        server.shutdown()
    finally:
        server.server_close()


def _resolve_rea_executable(root: Path) -> Path | None:
    """Return a usable path to the Rea executable.

    ``REA_BIN`` may point either directly to the executable or to a directory
    that contains it (common when users export ``REA_BIN=/usr/local/bin``).
    This helper normalises those inputs, falls back to the build output and
    finally checks the ``PATH`` using :func:`shutil.which`.
    """

    env_value = os.environ.get("REA_BIN")
    candidates: list[Path] = []

    if env_value:
        env_path = Path(env_value)
        if env_path.is_dir():
            candidates.append(env_path / "rea")
        candidates.append(env_path)

    candidates.append(root / "build" / "bin" / "rea")

    which_path = shutil.which("rea")
    if which_path:
        candidates.append(Path(which_path))

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate

    return None


def main() -> int:
    script_path = Path(__file__).resolve()
    root = find_repo_root(script_path)
    rea_bin = _resolve_rea_executable(root)
    if rea_bin is None:
        print(
            "Rea executable not found. Build the project or set REA_BIN to "
            "a valid executable path.",
            file=sys.stderr,
        )
        return 1

    server, base_url = start_server()
    tmp_dir = Path(tempfile.mkdtemp(prefix="rea_lib_tests_"))
    home_dir = tmp_dir / "home"
    home_dir.mkdir(parents=True, exist_ok=True)

    available_builtins = _discover_ext_builtins(rea_bin)
    env = build_env(root, tmp_dir, home_dir, base_url, available_builtins)

    test_program = script_path.parent / "library_tests.rea"
    cmd = [str(rea_bin), "--no-cache", str(test_program)]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=str(root))
    finally:
        stop_server(server)
        # Clean up the temporary directory after we're done.
        try:
            shutil.rmtree(tmp_dir)
        except Exception:
            # Leave the directory in place for debugging if removal fails.
            pass

    stdout = proc.stdout
    stderr = proc.stderr

    if stdout:
        print(stdout, end="")
    if stderr:
        print(stderr, file=sys.stderr, end="")

    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
