#!/usr/bin/env python3
"""Sync and launch the Qwen3-4B-Base Aether SFT run on Spark."""

from __future__ import annotations

import argparse
import pathlib
import shlex
import subprocess


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCAL_TRAIN_SCRIPT = REPO_ROOT / "tools" / "qwen3_base_lora_sft.py"
DEFAULT_HOST = "claw@100.124.15.16"
DEFAULT_WORKSPACE = "$HOME/training/aether-qwen3-base"


def run_ssh(host: str, script: str, *, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["ssh", host, f"bash -lc {shlex.quote(script)}"],
        text=True,
        input=input_text,
        capture_output=True,
        check=True,
    )


def sync_train_script(host: str, workspace: str) -> None:
    target = f"{workspace}/scripts/qwen3_base_lora_sft.py"
    script = f'mkdir -p "{workspace}/scripts" && cat > "{target}" && chmod +x "{target}"'
    run_ssh(host, script, input_text=LOCAL_TRAIN_SCRIPT.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--workspace", default=DEFAULT_WORKSPACE)
    parser.add_argument("--run-name", default="sft-seed-v1")
    parser.add_argument("--model-id", default="Qwen/Qwen3-4B-Base")
    parser.add_argument("--epochs", type=float, default=6.0)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--grad-accum", type=int, default=8)
    parser.add_argument("--max-seq-len", type=int, default=8192)
    parser.add_argument("--learning-rate", type=float, default=2e-4)
    parser.add_argument("--save-steps", type=int, default=20)
    parser.add_argument("--logging-steps", type=int, default=1)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("sync-script")
    subparsers.add_parser("start")
    subparsers.add_parser("status")

    args = parser.parse_args()

    if args.command == "sync-script":
        sync_train_script(args.host, args.workspace)
        print("synced")
        return 0

    if args.command == "status":
        remote_script = f"""
set -euo pipefail
workspace="{args.workspace}"
pidfile="$workspace/logs/{args.run_name}.pid"
logfile="$workspace/logs/{args.run_name}.log"
if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
  echo "running pid=$(cat "$pidfile")"
else
  echo "not_running"
fi
if [ -f "$logfile" ]; then
  echo "--- log tail ---"
  tail -n 40 "$logfile"
fi
"""
        proc = run_ssh(args.host, remote_script)
        print(proc.stdout, end="")
        return 0

    sync_train_script(args.host, args.workspace)
    remote_script = f"""
set -euo pipefail
workspace="{args.workspace}"
run_name="{args.run_name}"
pidfile="$workspace/logs/$run_name.pid"
logfile="$workspace/logs/$run_name.log"
outdir="$workspace/runs/$run_name"
mkdir -p "$workspace/logs" "$workspace/runs"
if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
  echo "already_running pid=$(cat "$pidfile")"
  exit 0
fi
nohup "$workspace/.venv/bin/python" "$workspace/scripts/qwen3_base_lora_sft.py" \
  --model-id '{args.model_id}' \
  --instruction-jsonl "$workspace/data/aether_instruction_sft.jsonl" \
  --repair-jsonl "$workspace/data/aether_repair_sft.jsonl" \
  --reference-json "$workspace/data/aether_reference_corpus.json" \
  --output-dir "$outdir" \
  --epochs {args.epochs} \
  --batch-size {args.batch_size} \
  --grad-accum {args.grad_accum} \
  --max-seq-len {args.max_seq_len} \
  --learning-rate {args.learning_rate} \
  --save-steps {args.save_steps} \
  --logging-steps {args.logging_steps} \
  >"$logfile" 2>&1 < /dev/null &
echo $! > "$pidfile"
echo "started pid=$(cat "$pidfile") run=$run_name"
"""
    proc = run_ssh(args.host, remote_script)
    print(proc.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
