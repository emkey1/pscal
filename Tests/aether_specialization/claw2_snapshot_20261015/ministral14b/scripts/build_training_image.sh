#!/usr/bin/env bash
set -euo pipefail
cd $HOME/training/ministral14b
# Based on the existing vLLM image already known-good on GB10/CUDA13.
docker build -t openclaw-ministral14b-train:latest -f Dockerfile .
