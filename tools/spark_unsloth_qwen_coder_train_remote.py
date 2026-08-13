#!/usr/bin/env python3
"""Sync and launch containerized Unsloth QLoRA runs for Qwen3-Coder-30B-A3B-Instruct."""

from __future__ import annotations

import argparse
import pathlib
import shlex
import subprocess


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
LOCAL_TRAIN_SCRIPT = REPO_ROOT / "tools" / "unsloth_qwen_coder_30b_sft.py"
LOCAL_SYNC_SCRIPT = REPO_ROOT / "tools" / "aether_specialization_sync_to_spark.py"
DEFAULT_HOST = "claw@100.124.15.16"
DEFAULT_WORKSPACE = "$HOME/training/aether-qwen-coder-30b-unsloth"
DEFAULT_NOTEBOOKS_REPO = "https://github.com/unslothai/notebooks.git"
DEFAULT_NOTEBOOKS_COMMIT = "568a161218dae1c30b6e13285192dc268850dc8b"
DEFAULT_IMAGE = "aether-unsloth-qwen3-coder-30b:568a161"
DEFAULT_MODEL_MIRROR_DIR = "$HOME/training/aether-qwen-coder-30b-unsloth/model-mirror"
DEFAULT_CONTAINER_MODEL_MIRROR_DIR = "/workspace/model-mirror"


def run_local(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, text=True, capture_output=True, check=True)


def run_ssh(host: str, script: str, *, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["ssh", host, f"bash -lc {shlex.quote(script)}"],
        text=True,
        input=input_text,
        capture_output=True,
        check=True,
    )


def resolve_remote_home(host: str) -> str:
    proc = subprocess.run(
        ["ssh", host, "bash -lc 'printf %s \"$HOME\"'"],
        text=True,
        capture_output=True,
        check=True,
    )
    return proc.stdout.strip()


def expand_remote_workspace(host: str, workspace: str) -> str:
    if "$HOME" not in workspace:
        return workspace
    return workspace.replace("$HOME", resolve_remote_home(host))


def remote_file_exists(host: str, path: str) -> bool:
    proc = subprocess.run(
        ["ssh", host, f"bash -lc {shlex.quote(f'test -f {shlex.quote(path)}')}"],
        text=True,
        capture_output=True,
    )
    return proc.returncode == 0


def remote_model_mirror_ready(host: str, mirror_dir: str) -> bool:
    required = [
        f"{mirror_dir}/config.json",
        f"{mirror_dir}/tokenizer.json",
        f"{mirror_dir}/model.safetensors.index.json",
    ]
    return all(remote_file_exists(host, path) for path in required)


def sync_assets(host: str, workspace: str) -> None:
    run_local(
        [
            "python3",
            str(LOCAL_SYNC_SCRIPT),
            "--host",
            host,
            "--remote-workspace",
            workspace,
            "--local-output-dir",
            str(REPO_ROOT / "Tests" / "aether_specialization" / "out"),
            "--aether-bin",
            str(REPO_ROOT / "build" / "bin" / "aether"),
        ]
    )


def sync_train_script(host: str, workspace: str) -> None:
    target = f"{workspace}/scripts/unsloth_qwen_coder_30b_sft.py"
    script = f'mkdir -p "{workspace}/scripts" && cat > "{target}" && chmod +x "{target}"'
    run_ssh(host, script, input_text=LOCAL_TRAIN_SCRIPT.read_text(encoding="utf-8"))


def sync_dockerfile(host: str, workspace: str, repo_url: str, commit: str) -> None:
    remote_script = f"""
set -euo pipefail
workspace="{workspace}"
repo_dir="$workspace/unsloth-notebooks"
if [ ! -d "$repo_dir/.git" ]; then
  git clone --depth 1 {shlex.quote(repo_url)} "$repo_dir"
fi
cd "$repo_dir"
git fetch --depth 1 origin {shlex.quote(commit)}
git checkout --detach {shlex.quote(commit)}
cp Dockerfile_DGX_Spark "$workspace/Dockerfile_DGX_Spark"
mkdir -p "$workspace/logs" "$workspace/runs" "$workspace/scripts"
"""
    run_ssh(host, remote_script)


def build_image(host: str, workspace: str, image: str, *, no_cache: bool) -> None:
    no_cache_flag = "--no-cache" if no_cache else ""
    remote_script = f"""
set -euo pipefail
workspace="{workspace}"
cd "$workspace"
docker build {no_cache_flag} -t {shlex.quote(image)} -f "$workspace/Dockerfile_DGX_Spark" "$workspace"
"""
    run_ssh(host, remote_script)


def start_run(
    *,
    host: str,
    workspace: str,
    image: str,
    run_name: str,
    model_id: str,
    model_path: str,
    epochs: float,
    learning_rate: float,
    batch_size: int,
    grad_accum: int,
    eval_cases: int,
    max_seq_length: int,
    export_gguf: str,
    merged_output_dir: str,
    storage_mount: str,
    allow_network_download: bool,
) -> None:
    container_name = f"aether-unsloth-{run_name}"
    max_seq_arg = f"--max-seq-length {max_seq_length}" if max_seq_length > 0 else ""
    export_gguf_arg = f"--export-gguf {shlex.quote(export_gguf)}" if export_gguf else ""
    merged_arg = f"--merged-output-dir {shlex.quote(merged_output_dir)}" if merged_output_dir else ""
    storage_mount_arg = f'-v {shlex.quote(f"{storage_mount}:{storage_mount}")}' if storage_mount else ""
    network_arg = "--allow-network-download" if allow_network_download else ""
    model_arg = shlex.quote(model_path if model_path else model_id)
    remote_script = f"""
set -euo pipefail
workspace="{workspace}"
container_name={shlex.quote(container_name)}
logfile="$workspace/logs/{run_name}.log"
outdir="$workspace/runs/{run_name}"
docker rm -f "$container_name" >/dev/null 2>&1 || true
mkdir -p "$workspace/logs" "$workspace/runs" "$workspace/data"
docker run -d \
  --name "$container_name" \
  --gpus all \
  --ipc=host \
  --shm-size 32g \
  -e HF_HUB_DISABLE_XET=1 \
  -v "$workspace:/workspace" \
  -v "$HOME/.cache/huggingface:/root/.cache/huggingface" \
  {storage_mount_arg} \
  -w /workspace \
  {shlex.quote(image)} \
  bash -lc '
    set -euo pipefail
    python /workspace/scripts/unsloth_qwen_coder_30b_sft.py \
      --model-id {model_arg} \
      --instruction-jsonl /workspace/data/aether_instruction_sft.jsonl \
      --repair-jsonl /workspace/data/aether_repair_sft.jsonl \
      --corpus-json /workspace/data/aether_raw_corpus.json \
      --reference-json /workspace/data/aether_reference_corpus.json \
      --output-dir /workspace/runs/{shlex.quote(run_name)} \
      --epochs {epochs} \
      --learning-rate {learning_rate} \
      --batch-size {batch_size} \
      --grad-accum {grad_accum} \
      --eval-cases {eval_cases} \
      {merged_arg} \
      {max_seq_arg} \
      {export_gguf_arg} \
      {network_arg} \
      2>&1 | tee /workspace/logs/{shlex.quote(run_name)}.log
  '
echo "started container=$container_name"
"""
    run_ssh(host, remote_script)


def prefetch_model(
    *,
    host: str,
    workspace: str,
    image: str,
    model_id: str,
    model_mirror_dir: str,
) -> None:
    container_mirror_dir = DEFAULT_CONTAINER_MODEL_MIRROR_DIR
    py_code = (
        "from huggingface_hub import snapshot_download; "
        f"print(snapshot_download(repo_id={model_id!r}, "
        f"local_dir={container_mirror_dir!r}, "
        "local_dir_use_symlinks=False, resume_download=True))"
    )
    remote_script = f"""
set -euo pipefail
workspace="{workspace}"
mirror_dir="{model_mirror_dir}"
mkdir -p "$mirror_dir"
docker run --rm \
  --gpus all \
  -e HF_HUB_DISABLE_XET=1 \
  -v "$HOME/.cache/huggingface:/root/.cache/huggingface" \
  -v "$workspace:/workspace" \
  -w /workspace \
  {shlex.quote(image)} \
  python -c {shlex.quote(py_code)}
"""
    run_ssh(host, remote_script)


def stop_run(host: str, run_name: str) -> None:
    container_name = f"aether-unsloth-{run_name}"
    run_ssh(
        host,
        f'docker rm -f {shlex.quote(container_name)} >/dev/null 2>&1 || true && echo "stopped {container_name}"',
    )


def show_status(host: str, workspace: str, run_name: str) -> str:
    container_name = f"aether-unsloth-{run_name}"
    remote_script = f"""
set -euo pipefail
workspace="{workspace}"
container_name={shlex.quote(container_name)}
logfile="$workspace/logs/{run_name}.log"
docker ps -a --filter "name=$container_name" --format "{{{{.Status}}}} {{{{.Names}}}}"
echo "---"
if [ -f "$logfile" ]; then
  tail -n 60 "$logfile"
fi
"""
    proc = run_ssh(host, remote_script)
    return proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--workspace", default=DEFAULT_WORKSPACE)
    parser.add_argument("--repo-url", default=DEFAULT_NOTEBOOKS_REPO)
    parser.add_argument("--repo-commit", default=DEFAULT_NOTEBOOKS_COMMIT)
    parser.add_argument("--image", default=DEFAULT_IMAGE)
    parser.add_argument("--run-name", default="sft-qwen-coder-30b-v1")
    parser.add_argument("--model-id", default="unsloth/Qwen3-Coder-30B-A3B-Instruct")
    parser.add_argument("--model-mirror-dir", default=DEFAULT_MODEL_MIRROR_DIR)
    parser.add_argument("--epochs", type=float, default=3.0)
    parser.add_argument("--learning-rate", type=float, default=1e-4)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--grad-accum", type=int, default=8)
    parser.add_argument("--eval-cases", type=int, default=12)
    parser.add_argument("--max-seq-length", type=int, default=0)
    parser.add_argument("--export-gguf", default="", help="comma-separated GGUF methods to export after training; empty disables")
    parser.add_argument("--storage-mount", default="/storage",
                        help="host path mounted into the container for the large merged export; empty disables the mount")
    parser.add_argument("--merged-output-dir", default="",
                        help="container path for the merged 16-bit checkpoint; defaults to <storage-mount>/aether-qwen-coder-30b-<run-name>/merged_16bit")
    parser.add_argument("--allow-network-download", action="store_true", help="allow transformers/huggingface to fetch missing model files")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("sync")
    build_parser = subparsers.add_parser("build-image")
    build_parser.add_argument("--no-cache", action="store_true")
    subparsers.add_parser("prefetch-model")
    subparsers.add_parser("start")
    subparsers.add_parser("status")
    subparsers.add_parser("stop")
    args = parser.parse_args()

    workspace = expand_remote_workspace(args.host, args.workspace)
    model_mirror_dir = expand_remote_workspace(args.host, args.model_mirror_dir)

    if args.command == "sync":
        sync_assets(args.host, args.workspace)
        sync_train_script(args.host, workspace)
        sync_dockerfile(args.host, workspace, args.repo_url, args.repo_commit)
        print("synced")
        return 0

    if args.command == "build-image":
        sync_assets(args.host, args.workspace)
        sync_train_script(args.host, workspace)
        sync_dockerfile(args.host, workspace, args.repo_url, args.repo_commit)
        build_image(args.host, workspace, args.image, no_cache=args.no_cache)
        print("built")
        return 0

    if args.command == "prefetch-model":
        prefetch_model(
            host=args.host,
            workspace=workspace,
            image=args.image,
            model_id=args.model_id,
            model_mirror_dir=model_mirror_dir,
        )
        print("prefetched")
        return 0

    if args.command == "status":
        print(show_status(args.host, workspace, args.run_name), end="")
        return 0

    if args.command == "stop":
        stop_run(args.host, args.run_name)
        return 0

    sync_assets(args.host, args.workspace)
    sync_train_script(args.host, workspace)
    sync_dockerfile(args.host, workspace, args.repo_url, args.repo_commit)
    build_image(args.host, workspace, args.image, no_cache=False)
    model_path = DEFAULT_CONTAINER_MODEL_MIRROR_DIR if remote_model_mirror_ready(args.host, model_mirror_dir) else ""

    merged_output_dir = args.merged_output_dir
    if not merged_output_dir and args.storage_mount:
        merged_output_dir = f"{args.storage_mount}/aether-qwen-coder-30b-{args.run_name}/merged_16bit"

    start_run(
        host=args.host,
        workspace=workspace,
        image=args.image,
        run_name=args.run_name,
        model_id=args.model_id,
        model_path=model_path,
        epochs=args.epochs,
        learning_rate=args.learning_rate,
        batch_size=args.batch_size,
        grad_accum=args.grad_accum,
        eval_cases=args.eval_cases,
        max_seq_length=args.max_seq_length,
        export_gguf=args.export_gguf,
        merged_output_dir=merged_output_dir,
        storage_mount=args.storage_mount,
        allow_network_download=args.allow_network_download,
    )
    print(show_status(args.host, workspace, args.run_name), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
