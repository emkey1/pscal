#!/usr/bin/env bash
set -euo pipefail

workspace_root="${1:-$HOME/training/aether-qwen3-base}"
python_bin="${PYTHON_BIN:-python3}"

mkdir -p "$workspace_root"
mkdir -p "$workspace_root"/{configs,data,logs,runs,scripts}

"$python_bin" -m venv "$workspace_root/.venv"
source "$workspace_root/.venv/bin/activate"

python -m pip install --upgrade pip wheel setuptools
python -m pip install \
  transformers \
  datasets \
  accelerate \
  peft \
  trl \
  sentencepiece \
  safetensors \
  jinja2 \
  pyyaml

cat > "$workspace_root/README.md" <<'EOF'
# Aether Qwen3-4B-Base Workspace

This workspace is for compiler-verified Aether specialization runs.

Suggested stages:

1. export raw Aether corpus from the repo
2. build verified instruction and repair datasets
3. benchmark untuned base model
4. continued pretraining on raw Aether
5. SFT on prompt-to-Aether pairs
6. repair SFT
7. preference optimization

Do not train on prose-only Aether guides as the primary signal.
EOF

echo "workspace ready: $workspace_root"
