#!/usr/bin/env python3
"""Run the opt-in CLike library test suite."""

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
    return script_path.resolve().parents[3]


def _discover_ext_builtins(executable: Path) -> set[str]:
    """Return the set of extended builtin categories exposed by *executable*."""

    try:
        proc = subprocess.run(
            [str(executable), "--dump-ext-builtins"],
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
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
    json_path: Path,
    available_builtins: set[str],
) -> dict[str, str]:
    """Construct the environment for running the CLike test program."""
    env = os.environ.copy()

    lib_dir = root / "lib" / "clike"
    env.setdefault("CLIKE_LIB_DIR", str(lib_dir))

    env["CLIKE_TEST_TMPDIR"] = str(tmp_dir)
    env["CLIKE_TEST_HTTP_BASE_URL"] = base_url
    env["CLIKE_TEST_JSON_PATH"] = str(json_path)
    env["HOME"] = str(home_dir)

    if available_builtins:
        env["CLIKE_TEST_EXT_BUILTINS"] = ",".join(sorted(available_builtins))
        env["CLIKE_TEST_HAS_YYJSON"] = "1" if "yyjson" in available_builtins else "0"

    return env


class _RequestHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args) -> None:  # noqa: A003 - match BaseHTTPRequestHandler signature
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
    # Bind to loopback on an ephemeral port to avoid permission issues on systems that
    # block low ports in sandboxed environments.
    server = socketserver.TCPServer(("127.0.0.1", 0), _RequestHandler, bind_and_activate=False)
    server.allow_reuse_address = True
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            server.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            # Not fatal; fall back to default reuse settings.
            pass
    try:
        server.server_bind()
        server.server_activate()
    except PermissionError:
        server.server_close()
        return None, ""
    except Exception:
        server.server_close()
        raise
    host, port = server.server_address
    base_url = f"http://{host}:{port}"
    thread = threading.Thread(target=server.serve_forever, name="clike-http-server", daemon=True)
    thread.start()
    return server, base_url


def stop_server(server: socketserver.TCPServer) -> None:
    """Shut down the local HTTP server."""
    if server is None:
        return
    try:
        server.shutdown()
    finally:
        server.server_close()


def _resolve_clike_executable(root: Path) -> Path | None:
    """Return a usable path to the CLike executable."""

    env_value = os.environ.get("CLIKE_BIN")
    candidates: list[Path] = []

    if env_value:
        env_path = Path(env_value)
        if env_path.is_dir():
            candidates.append(env_path / "clike")
        candidates.append(env_path)

    candidates.append(root / "build" / "bin" / "clike")

    which_path = shutil.which("clike")
    if which_path:
        candidates.append(Path(which_path))

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate

    return None


def _create_json_fixture(tmp_dir: Path) -> Path:
    """Write a JSON document used by the suite and return its path."""
    fixture_path = tmp_dir / "fixture.json"
    fixture_path.write_text(
        '{"name": "pscal", "version": 2, "enabled": true, '
        '"threshold": 3.5, "features": ["http", "json", "strings"], "missing": null}\n',
        encoding="utf-8",
    )
    return fixture_path


def main() -> int:
    script_path = Path(__file__).resolve()
    root = find_repo_root(script_path)
    clike_bin = _resolve_clike_executable(root)
    if clike_bin is None:
        print(
            "Clike executable not found. Build the project or set CLIKE_BIN to "
            "a valid executable path.",
            file=sys.stderr,
        )
        return 1

    server, base_url = start_server()
    if server is None:
        print("Skipping CLike library http tests: loopback bind not permitted.", file=sys.stderr)
        return 0
    tmp_dir = Path(tempfile.mkdtemp(prefix="clike_lib_tests_"))
    home_dir = tmp_dir / "home"
    home_dir.mkdir(parents=True, exist_ok=True)
    json_fixture = _create_json_fixture(tmp_dir)

    available_builtins = _discover_ext_builtins(clike_bin)
    env = build_env(root, tmp_dir, home_dir, base_url, json_fixture, available_builtins)

    test_program = script_path.parent / "library_tests.cl"
    cmd = [str(clike_bin), "--no-cache", str(test_program)]

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            cwd=str(root),
        )
    finally:
        stop_server(server)
        try:
            shutil.rmtree(tmp_dir)
        except Exception:
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
