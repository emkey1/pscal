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
import time
from concurrent.futures import ThreadPoolExecutor
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


def _capture_ext_builtin_dump(executable: Path) -> str:
    """Return the raw ``--dump-ext-builtins`` output for *executable*."""

    try:
        proc = subprocess.run(
            [str(executable), "--dump-ext-builtins"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return ""
    return proc.stdout


def _parse_ext_builtin_dump(output: str) -> dict[str, object]:
    """Parse the dump output into ordered collections for comparison."""

    categories: list[str] = []
    groups: dict[str, list[str]] = {}
    functions: dict[tuple[str, str], list[str]] = {}

    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        parts = line.split()
        tag = parts[0]
        if tag == "category" and len(parts) >= 2:
            category = parts[1]
            categories.append(category)
            groups.setdefault(category, [])
        elif tag == "group" and len(parts) >= 3:
            category, group = parts[1], parts[2]
            groups.setdefault(category, []).append(group)
        elif tag == "function" and len(parts) >= 4:
            category, group, func = parts[1], parts[2], parts[3]
            functions.setdefault((category, group), []).append(func)

    ordered_groups = {cat: tuple(groups.get(cat, [])) for cat in categories}
    ordered_functions = {key: tuple(funcs) for key, funcs in functions.items()}
    return {
        "categories": tuple(categories),
        "groups": ordered_groups,
        "functions": ordered_functions,
    }


def _validate_ext_builtin_dump(executable: Path) -> bool:
    """Verify enumeration stability and capture simple timing data."""

    baseline_output = _capture_ext_builtin_dump(executable)
    if not baseline_output:
        return True

    baseline = _parse_ext_builtin_dump(baseline_output)

    # Sequential consistency: repeated runs should match exactly.
    for attempt in range(3):
        candidate = _parse_ext_builtin_dump(_capture_ext_builtin_dump(executable))
        if candidate != baseline:
            print(
                "rea --dump-ext-builtins order changed between runs",
                file=sys.stderr,
            )
            return False

    # Concurrent reads should expose the same inventory and order.
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(_capture_ext_builtin_dump, [executable] * 4))
    if any(result != baseline_output for result in results):
        print(
            "rea --dump-ext-builtins produced inconsistent output under load",
            file=sys.stderr,
        )
        return False

    # Lightweight timing harness to document lookup improvements.
    iterations = 8
    start = time.perf_counter()
    for _ in range(iterations):
        _capture_ext_builtin_dump(executable)
    elapsed = time.perf_counter() - start
    avg_ms = (elapsed / iterations) * 1000 if iterations else 0.0
    print(f"[rea] ext builtin dump: {iterations} runs, avg {avg_ms:.2f} ms")

    return True


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
    # Bind explicitly to 127.0.0.1 on an ephemeral port to avoid sandboxed port restrictions.
    server = socketserver.TCPServer(("127.0.0.1", 0), _RequestHandler, bind_and_activate=False)
    server.allow_reuse_address = True
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            server.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
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
    thread = threading.Thread(target=server.serve_forever, name="rea-http-server", daemon=True)
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

    if not _validate_ext_builtin_dump(rea_bin):
        return 1

    server, base_url = start_server()
    if server is None:
        print("Skipping Rea library http tests: loopback bind not permitted.", file=sys.stderr)
        return 0
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
