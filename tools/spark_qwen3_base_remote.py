#!/usr/bin/env python3
"""Manage and query a remote Qwen3-4B-Base server on Spark."""

from __future__ import annotations

import argparse
import json
import pathlib
import shlex
import subprocess
import time
import urllib.error
import urllib.request


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCAL_SERVER_SCRIPT = REPO_ROOT / "tools" / "qwen3_base_server.py"
DEFAULT_HOST = "claw@100.124.15.16"
DEFAULT_WORKSPACE = "$HOME/training/aether-qwen3-base"
DEFAULT_PORT = 18081
DEFAULT_MODEL_ID = "Qwen/Qwen3-4B-Base"


def run_ssh(host: str, script: str, *, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["ssh", host, f"bash -lc {shlex.quote(script)}"],
        text=True,
        input=input_text,
        capture_output=True,
        check=True,
    )


def sync_server(host: str, workspace: str) -> None:
    target = f"{workspace}/scripts/qwen3_base_server.py"
    script = f'mkdir -p "{workspace}/scripts" && cat > "{target}" && chmod +x "{target}"'
    run_ssh(host, script, input_text=LOCAL_SERVER_SCRIPT.read_text(encoding="utf-8"))


def build_base_url(host: str, port: int) -> str:
    visible_host = host.split("@", 1)[-1]
    return f"http://{visible_host}:{port}"


def http_get_json(url: str, timeout: int = 5) -> dict[str, object]:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def http_post_json(url: str, payload: dict[str, object], timeout: int) -> dict[str, object]:
    body = json.dumps(payload, ensure_ascii=True).encode("utf-8")
    request = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def remote_generate(
    *,
    host: str,
    port: int,
    prompt: str,
    max_new_tokens: int,
    temperature: float,
    timeout_seconds: int,
) -> dict[str, object]:
    payload = json.dumps(
        {
            "prompt": prompt,
            "max_new_tokens": max_new_tokens,
            "temperature": temperature,
        },
        ensure_ascii=True,
    )
    remote_script = f"""
set -euo pipefail
tmp="$(mktemp)"
cat > "$tmp"
"$HOME/training/aether-qwen3-base/.venv/bin/python" - <<'PY' "$tmp" {port} {timeout_seconds}
import json
import pathlib
import sys
import urllib.request

payload_path = pathlib.Path(sys.argv[1])
port = int(sys.argv[2])
timeout_seconds = int(sys.argv[3])
payload = payload_path.read_bytes()
request = urllib.request.Request(
    f"http://127.0.0.1:{{port}}/generate",
    data=payload,
    headers={{"Content-Type": "application/json"}},
)
with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
rm -f "$tmp"
"""
    proc = run_ssh(host, remote_script, input_text=payload)
    return json.loads(proc.stdout)


def start_server(host: str, workspace: str, port: int, model_id: str) -> None:
    start_server_with_adapter(host, workspace, port, model_id, "")


def start_server_with_adapter(host: str, workspace: str, port: int, model_id: str, adapter_path: str) -> None:
    sync_server(host, workspace)
    adapter_arg = ""
    if adapter_path:
        adapter_arg = f'  --adapter-path "{adapter_path}" \\\n'
    remote_script = f"""
set -euo pipefail
workspace="{workspace}"
pidfile="$workspace/logs/qwen3_base_server.pid"
logfile="$workspace/logs/qwen3_base_server.log"
mkdir -p "$workspace/logs"
if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
  echo "already_running"
  exit 0
fi
nohup "$workspace/.venv/bin/python" "$workspace/scripts/qwen3_base_server.py" \
  --model-id '{model_id}' \
{adapter_arg}\
  --host 0.0.0.0 \
  --port {port} \
  >"$logfile" 2>&1 < /dev/null &
echo $! > "$pidfile"
echo "started pid=$(cat "$pidfile")"
"""
    run_ssh(host, remote_script)


def stop_server(host: str, workspace: str) -> None:
    remote_script = f"""
set -euo pipefail
workspace="{workspace}"
pidfile="$workspace/logs/qwen3_base_server.pid"
if [ -f "$pidfile" ]; then
  pid="$(cat "$pidfile")"
  kill "$pid" 2>/dev/null || true
  rm -f "$pidfile"
  echo "stopped"
else
  echo "not_running"
fi
"""
    run_ssh(host, remote_script)


def poll_health(base_url: str, timeout_seconds: int) -> dict[str, object]:
    deadline = time.time() + timeout_seconds
    last_error = "server did not become healthy"
    while time.time() < deadline:
        try:
            return http_get_json(base_url + "/health", timeout=5)
        except Exception as exc:  # pragma: no cover - polling path
            last_error = str(exc)
            time.sleep(2)
    raise RuntimeError(last_error)


def health_matches_requested(
    health: dict[str, object],
    *,
    model_id: str,
    adapter_path: str,
) -> bool:
    running_model = str(health.get("model") or "")
    running_adapter = str(health.get("adapter_path") or "")
    return running_model == model_id and running_adapter == adapter_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--workspace", default=DEFAULT_WORKSPACE)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--model-id", default=DEFAULT_MODEL_ID)
    parser.add_argument("--adapter-path", default="")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("sync-server")

    start = subparsers.add_parser("start-server")
    start.add_argument("--wait-seconds", type=int, default=900)

    subparsers.add_parser("stop-server")
    subparsers.add_parser("status")

    generate = subparsers.add_parser("generate")
    generate.add_argument("--prompt-file", type=pathlib.Path, required=True)
    generate.add_argument("--max-new-tokens", type=int, default=3000)
    generate.add_argument("--temperature", type=float, default=0.0)
    generate.add_argument("--timeout-seconds", type=int, default=300)
    generate.add_argument("--raw", action="store_true")

    args = parser.parse_args()
    base_url = build_base_url(args.host, args.port)

    if args.command == "sync-server":
        sync_server(args.host, args.workspace)
        print("synced")
        return 0

    if args.command == "start-server":
        try:
            health = http_get_json(base_url + "/health", timeout=5)
        except Exception:
            start_server_with_adapter(
                args.host,
                args.workspace,
                args.port,
                args.model_id,
                args.adapter_path.strip(),
            )
            health = poll_health(base_url, args.wait_seconds)
        else:
            requested_adapter = args.adapter_path.strip()
            if not health_matches_requested(
                health,
                model_id=args.model_id,
                adapter_path=requested_adapter,
            ):
                stop_server(args.host, args.workspace)
                start_server_with_adapter(
                    args.host,
                    args.workspace,
                    args.port,
                    args.model_id,
                    requested_adapter,
                )
                health = poll_health(base_url, args.wait_seconds)
        print(json.dumps(health, ensure_ascii=True))
        return 0

    if args.command == "stop-server":
        stop_server(args.host, args.workspace)
        return 0

    if args.command == "status":
        print(json.dumps(http_get_json(base_url + "/health"), ensure_ascii=True))
        return 0

    if args.command == "generate":
        prompt = args.prompt_file.read_text(encoding="utf-8")
        result = remote_generate(
            host=args.host,
            port=args.port,
            prompt=prompt,
            max_new_tokens=args.max_new_tokens,
            temperature=args.temperature,
            timeout_seconds=args.timeout_seconds,
        )
        if args.raw:
            print(result["raw_text"], end="")
        else:
            print(json.dumps(result, ensure_ascii=True))
        return 0

    raise RuntimeError(f"unsupported command {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
