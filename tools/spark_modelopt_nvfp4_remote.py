#!/usr/bin/env python3
"""Prepare and run NVFP4 export jobs for the Spark host."""

from __future__ import annotations

import argparse
import pathlib
import shlex
import subprocess


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCAL_EXPORT_SCRIPT = REPO_ROOT / "tools" / "modelopt_nvfp4_export.py"
DEFAULT_HOST = "claw@100.124.15.16"
DEFAULT_IMAGE = "aether-unsloth-qwen3-coder-30b:568a161"
DEFAULT_WORKSPACE = "/storage/aether-qwen-coder-30b-unsloth-nvfp4"
DEFAULT_SOURCE_MODEL = (
    "/storage/aether-qwen-coder-30b-unsloth/runs/sft-qwen-coder-30b-v1/final/merged-bf16"
)
DEFAULT_INSTRUCTION_JSONL = str(
    REPO_ROOT / "Tests" / "aether_specialization" / "out" / "aether_instruction_sft.jsonl"
)
DEFAULT_REPAIR_JSONL = str(
    REPO_ROOT / "Tests" / "aether_specialization" / "out" / "aether_repair_sft.jsonl"
)


def run_ssh(host: str, script: str, *, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["ssh", host, f"bash -lc {shlex.quote(script)}"],
        text=True,
        input=input_text,
        capture_output=True,
        check=True,
    )


def sync_file(host: str, remote_path: str, local_path: pathlib.Path) -> None:
    remote_script = (
        f'mkdir -p "{pathlib.PurePosixPath(remote_path).parent}" '
        f'&& cat > "{remote_path}"'
    )
    run_ssh(host, remote_script, input_text=local_path.read_text(encoding="utf-8"))


def prepare_workspace(host: str, workspace: str, source_model: str) -> None:
    remote_script = f"""
set -euo pipefail
workspace={shlex.quote(workspace)}
source_model={shlex.quote(source_model)}
mkdir -p "$workspace/scripts" "$workspace/logs" "$workspace/data" "$workspace/exports"
if [ ! -d "$source_model" ]; then
  echo "missing source model: $source_model" >&2
  exit 1
fi
du -sh "$source_model"
"""
    run_ssh(host, remote_script)


def setup_venv(host: str, workspace: str, image: str) -> None:
    remote_script = f"""
set -euo pipefail
workspace={shlex.quote(workspace)}
docker run --rm \
  -e WORKSPACE="$workspace" \
  -v /storage:/storage \
  busybox \
  sh -lc 'rm -rf "$WORKSPACE/.venv"'
docker run --rm \
  --gpus all \
  --ipc=host \
  --shm-size 32g \
  -v /storage:/storage \
  -v "$HOME/.cache/huggingface:/root/.cache/huggingface" \
  -e WORKSPACE="$workspace" \
  -w "$workspace" \
  {shlex.quote(image)} \
  bash -lc '
    set -euo pipefail
    export PIP_CACHE_DIR="$WORKSPACE/pip-cache"
    python -m venv --system-site-packages "$WORKSPACE/.venv"
    . "$WORKSPACE/.venv/bin/activate"
    python -m pip install -U pip wheel setuptools
    python -m pip install -U "nvidia-modelopt[hf]" triton
    python - <<'"'"'PY'"'"'
import modelopt
import torch
import transformers
print("modelopt", getattr(modelopt, "__version__", "unknown"))
print("torch", torch.__version__)
print("transformers", transformers.__version__)
PY
  '
"""
    run_ssh(host, remote_script)


def start_export(
    *,
    host: str,
    workspace: str,
    image: str,
    source_model: str,
    run_name: str,
    qformat: str,
    max_samples: int,
    max_seq_length: int,
) -> None:
    container_name = f"aether-modelopt-{run_name}"
    export_dir = f"{workspace}/exports/{run_name}"
    remote_script = f"""
set -euo pipefail
workspace={shlex.quote(workspace)}
source_model={shlex.quote(source_model)}
container_name={shlex.quote(container_name)}
docker rm -f "$container_name" >/dev/null 2>&1 || true
mkdir -p "$workspace/logs" "$workspace/exports" "{export_dir}"
docker run -d \
  --name "$container_name" \
  --gpus all \
  --ipc=host \
  --shm-size 32g \
  -v /storage:/storage \
  -v "$HOME/.cache/huggingface:/root/.cache/huggingface" \
  -e WORKSPACE="$workspace" \
  -e SOURCE_MODEL="$source_model" \
  -w "$workspace" \
  {shlex.quote(image)} \
  bash -lc '
    set -euo pipefail
    . "$WORKSPACE/.venv/bin/activate"
    python "$WORKSPACE/scripts/modelopt_nvfp4_export.py" \
      --model-path "$SOURCE_MODEL" \
      --export-dir "{export_dir}" \
      --calibration-jsonl "$WORKSPACE/data/aether_instruction_sft.jsonl" \
      --calibration-jsonl "$WORKSPACE/data/aether_repair_sft.jsonl" \
      --qformat {shlex.quote(qformat)} \
      --max-samples {max_samples} \
      --max-seq-length {max_seq_length} \
      2>&1 | tee "$WORKSPACE/logs/{run_name}.log"
  '
echo "started container=$container_name"
"""
    run_ssh(host, remote_script)


def show_status(host: str, workspace: str, run_name: str) -> str:
    container_name = f"aether-modelopt-{run_name}"
    remote_script = f"""
set -euo pipefail
workspace={shlex.quote(workspace)}
container_name={shlex.quote(container_name)}
logfile="$workspace/logs/{run_name}.log"
docker ps -a --filter "name=$container_name" --format "{{{{.Status}}}} {{{{.Names}}}}"
echo "---"
if [ -f "$logfile" ]; then
  tail -n 80 "$logfile"
fi
"""
    proc = run_ssh(host, remote_script)
    return proc.stdout


def stop_run(host: str, run_name: str) -> None:
    container_name = f"aether-modelopt-{run_name}"
    run_ssh(
        host,
        f'docker rm -f {shlex.quote(container_name)} >/dev/null 2>&1 || true && echo "stopped {container_name}"',
    )


def prune_root_copies(host: str) -> None:
    remote_script = """
set -euo pipefail
root_ws="$HOME/training/aether-qwen-coder-30b-unsloth"
docker run --rm \
  -v "$root_ws:/target" \
  busybox \
  sh -lc 'rm -rf /target/runs/sft-qwen-coder-30b-v1/final/gguf /target/model-mirror'
df -h /
"""
    run_ssh(host, remote_script)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--workspace", default=DEFAULT_WORKSPACE)
    parser.add_argument("--image", default=DEFAULT_IMAGE)
    parser.add_argument("--source-model", default=DEFAULT_SOURCE_MODEL)
    parser.add_argument("--run-name", default="qwen-coder-30b-nvfp4-v1")
    parser.add_argument("--qformat", default="nvfp4_experts_only")
    parser.add_argument("--max-samples", type=int, default=192)
    parser.add_argument("--max-seq-length", type=int, default=1024)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("prepare")
    subparsers.add_parser("setup-venv")
    subparsers.add_parser("start")
    subparsers.add_parser("status")
    subparsers.add_parser("stop")
    subparsers.add_parser("prune-root-copies")
    args = parser.parse_args()

    if args.command == "prepare":
        prepare_workspace(args.host, args.workspace, args.source_model)
        sync_file(args.host, f"{args.workspace}/scripts/modelopt_nvfp4_export.py", LOCAL_EXPORT_SCRIPT)
        sync_file(
            args.host,
            f"{args.workspace}/data/aether_instruction_sft.jsonl",
            pathlib.Path(DEFAULT_INSTRUCTION_JSONL),
        )
        sync_file(
            args.host,
            f"{args.workspace}/data/aether_repair_sft.jsonl",
            pathlib.Path(DEFAULT_REPAIR_JSONL),
        )
        print("prepared")
        return 0

    if args.command == "setup-venv":
        setup_venv(args.host, args.workspace, args.image)
        print("venv ready")
        return 0

    if args.command == "start":
        start_export(
            host=args.host,
            workspace=args.workspace,
            image=args.image,
            source_model=args.source_model,
            run_name=args.run_name,
            qformat=args.qformat,
            max_samples=args.max_samples,
            max_seq_length=args.max_seq_length,
        )
        print("started")
        return 0

    if args.command == "status":
        print(show_status(args.host, args.workspace, args.run_name))
        return 0

    if args.command == "stop":
        stop_run(args.host, args.run_name)
        print("stopped")
        return 0

    if args.command == "prune-root-copies":
        prune_root_copies(args.host)
        print("pruned")
        return 0

    raise AssertionError("unreachable")


if __name__ == "__main__":
    raise SystemExit(main())
