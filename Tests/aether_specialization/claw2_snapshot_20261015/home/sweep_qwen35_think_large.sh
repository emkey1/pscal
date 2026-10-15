#!/bin/bash
# Scoped test: does qwen35-9b-base's guided performance on the `large` suite
# (its biggest full-guide gap vs qwen3-8b-nothink: 5/9 vs 9/9) improve with
# thinking enabled, vs the existing no-think result? One suite, both guide
# variants, repair=2 -- not the full 3-suite sweep, to bound wall-clock given
# reasoning mode's known ~10-20x slowdown on this project's models.
set -u
BIN=$HOME/aether-current/build/aether
HARNESS=$HOME/pscal-bench/tools/aether_doc_bench.py
TDIR=$HOME/pscal-bench/Tests/aether_doc_bench
OUT=$HOME/guided_sweep_base
DEST=$OUT/dest.json   # thinking ON (no chat_template_kwargs override) -- already exists
TAG=qwen35-9b-base-think
MODEL_PATH=/storage/models/Qwen3.5-9B

teardown(){ docker stop -t 120 vllm-sweep >/dev/null 2>&1; docker rm -f vllm-sweep >/dev/null 2>&1; sleep 10
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1
  for i in $(seq 1 20); do
    avail=$(free -m | awk '/Mem:/{print $7}')
    [ "${avail:-0}" -ge 100000 ] && break
    sleep 15
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1
  done; }
serve(){ teardown
  docker run -d --name vllm-sweep --gpus all --ipc=host -p 8000:8000 -v /storage:/storage -v /slows:/slows \
    vllm/vllm-openai:latest --model "$1" --served-model-name guided-model \
    --max-model-len 40960 --gpu-memory-utilization 0.80 --enforce-eager --trust-remote-code >/dev/null 2>&1; }
ready(){ for i in $(seq 1 40); do
    docker logs vllm-sweep 2>&1 | grep -qaiE "Application startup complete" && return 0
    docker logs vllm-sweep 2>&1 | grep -qaiE "EngineCore failed to start|out of memory|no kernel image|ValueError: Free memory|Cannot allocate|torch.cuda.OutOfMemory" && return 1
    docker ps -a --filter name=vllm-sweep --format '{{.Status}}' | grep -qi Exited && return 1
    sleep 30; done; return 1; }

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1; sleep 3
avail=$(free -m | awk '/Mem:/{print $7}')
echo "==== QWEN35-9B THINK-MODE (large only) START $(date) — ${avail}MB available ===="
if [ "${avail:-0}" -lt 40000 ]; then
  echo ">>> ABORT: only ${avail}MB free."
  exit 1
fi

echo "[$TAG] serving $(date +%H:%M)"; serve "$MODEL_PATH"
if ready; then
  echo "[$TAG] READY, benching guided (full,small) large only (repair=2)"
  python3 "$HARNESS" --tasks "$TDIR/tasks_hard.json" --docs full,small --repair-attempts 2 \
    --destinations-config "$DEST" --aether-bin "$BIN" \
    --output-json "$OUT/${TAG}_large.json" >"$OUT/${TAG}_large.log" 2>&1
  echo "[$TAG] large done $(date +%H:%M)"
else
  echo "[$TAG] SERVE FAILED :: $(docker logs vllm-sweep 2>&1 | grep -aiE 'error|memory|not supported|EngineCore|assert' | tail -1)"
fi
teardown
echo "==== QWEN35-9B THINK-MODE DONE $(date) ===="
