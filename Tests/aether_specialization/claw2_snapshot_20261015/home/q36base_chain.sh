#!/bin/bash
# Clean qwen3.6-35b-a3b BASE pipeline: wait for download -> bf16 GGUF (lossless,
# no quant corruption of the hybrid linear-attention tensors) -> import to claw1 ollama.
set -u
echo "=== chain start $(date) ==="

# 1. wait for the download container to finish
while docker ps --filter name=qwen36dl --format '{{.Status}}' | grep -qi Up; do sleep 60; done
if ! docker logs qwen36dl 2>&1 | grep -q "DOWNLOAD COMPLETE"; then
  echo "!!! download did NOT complete — inspect: docker logs qwen36dl"; exit 1
fi
echo "=== download complete $(date), size $(du -sh /storage/qwen36-base 2>/dev/null|cut -f1) ==="

# 2. convert to bf16 GGUF (CPU-only; convert is pure-python, no GPU needed)
mkdir -p /storage/gguf
docker rm -f qwen36conv >/dev/null 2>&1
docker run --rm --name qwen36conv -v /storage:/storage -v $HOME/llama.cpp:/llama gguf-convert \
  bash -c "cd /llama && python3 convert_hf_to_gguf.py /storage/qwen36-base --outfile /storage/gguf/qwen36-base-bf16.gguf --outtype bf16" 2>&1 | tail -6
if [ ! -s /storage/gguf/qwen36-base-bf16.gguf ]; then echo "!!! CONVERT FAILED"; exit 1; fi
echo "=== bf16 GGUF ready: $(ls -lh /storage/gguf/qwen36-base-bf16.gguf|awk '{print $5}') $(date) ==="

# 3. import to claw1 ollama (best-effort; GGUF persists on /storage either way)
if ssh -o BatchMode=yes -o ConnectTimeout=10 claw1 "printf 'FROM /storage/gguf/qwen36-base-bf16.gguf\n' > /storage/gguf/Modelfile.q36base && ollama create qwen3.6-35b-a3b -f /storage/gguf/Modelfile.q36base" 2>&1 | tail -2; then
  echo "=== IMPORTED to claw1 ollama as 'qwen3.6-35b-a3b' $(date) ==="
else
  echo "=== import needs manual run from m5t: GGUF is at /storage/gguf/qwen36-base-bf16.gguf ==="
fi
echo "=== Q36-BASE CHAIN DONE $(date) ==="
