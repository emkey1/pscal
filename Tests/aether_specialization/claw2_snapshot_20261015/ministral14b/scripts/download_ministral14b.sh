#!/usr/bin/env bash
set -euo pipefail
MODEL_ID="mistralai/Ministral-3-14B-Reasoning-2512"
DEST="$HOME/models/hf/mistralai/Ministral-3-14B-Reasoning-2512"
LOG="$HOME/logs/download-ministral14b.log"
mkdir -p "$(dirname "$DEST")" $HOME/logs
python3 -m venv $HOME/venvs/hf || true
source $HOME/venvs/hf/bin/activate
python -m pip install -U pip huggingface_hub hf_transfer
export HF_HUB_ENABLE_HF_TRANSFER=1
python - <<PY 2>&1 | tee "$LOG"
from huggingface_hub import snapshot_download
snapshot_download(
    repo_id="$MODEL_ID",
    local_dir="$DEST",
    local_dir_use_symlinks=False,
    resume_download=True,
)
print("Downloaded", "$MODEL_ID", "to", "$DEST")
PY
