#!/bin/bash
# Serve + benchmark one cs-aug18-precision-grid model: simple/large/cs, no-guide
# (--docs none), matching the cs-aug4 board convention (temperature 0.2, seed 42,
# repair-attempts 1). Serves via vLLM on claw2 (serve_any.sh, port 8019), frees the
# GPU afterward. Run from the repo root (PBuild/).
#
# Usage: ./Tests/aether_doc_bench/bench_cs_aug18_precision.sh <tag>
#   e.g. ./Tests/aether_doc_bench/bench_cs_aug18_precision.sh qwen3-8b-nothink-4bit-cs-aug18
set -euo pipefail

TAG=${1:?usage: bench_cs_aug18_precision.sh <tag>}
DEST="claw2-$TAG"
CFG=Tests/aether_doc_bench/destinations.cs_aug18_precision.json
OUTDIR=Tests/aether_doc_bench/out/cs_aug18_precision
mkdir -p "$OUTDIR"

echo "=== serving $TAG on claw2 (vLLM, port 8019) ==="
# 0.85 (serve_any.sh's default) requests 103GiB against a 121.63GiB unified-memory pool;
# claw2 typically has ~29GiB in host-side use at any given moment, leaving too little
# free for vLLM's startup check. 0.65 (~79GiB) comfortably covers a 9B model's weights
# + KV cache at max-model-len 16384 with headroom for host-side memory pressure.
ssh claw@claw2 "cd ~/training/aether-qwen-coder-30b-unsloth && ./serve_any.sh $TAG 16384 0.65"

echo "=== waiting for vLLM readiness ==="
for i in $(seq 1 60); do
  if curl -sf "http://claw2.tailfe3968.ts.net:8019/v1/models" >/dev/null 2>&1; then
    echo "ready after ${i}0s"
    break
  fi
  sleep 10
done

for suite in simple large cs; do
  case "$suite" in
    simple) tasks=Tests/aether_doc_bench/tasks_v2_pos.json ;;
    large)  tasks=Tests/aether_doc_bench/tasks_hard.json ;;
    cs)     tasks=Tests/aether_doc_bench/tasks_cs.json ;;
  esac
  echo "=== $TAG / $suite ==="
  python3 tools/aether_doc_bench.py \
    --tasks "$tasks" \
    --docs none \
    --repair-attempts 1 \
    --destinations-config "$CFG" \
    --destination "$DEST" \
    --output-json "$OUTDIR/${TAG}_${suite}.json" \
    --text-summary --progress
done

echo "=== freeing GPU: stopping vLLM container ==="
ssh claw@claw2 "docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1 || true"
echo "=== done: $TAG ==="
