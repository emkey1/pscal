#!/bin/bash
# claw2-native, resumable, no-guide sweep of trained Aether models on v2 + large + cs.
# Hardened: memory precheck, graceful container stop (no GPU-mem leak), abort if the GPU
# is wedged instead of burning the whole list.
set -u
BIN=$HOME/aether-current/build/aether
HARNESS=$HOME/pscal-bench/tools/aether_doc_bench.py
TDIR=$HOME/pscal-bench/Tests/aether_doc_bench
OUT=$HOME/trained_sweep; mkdir -p "$OUT"
DEST=$OUT/dest.json
cat > "$DEST" <<'JSON'
{"destinations":[{"id":"aether-model","type":"openai_chat_completions","base_url":"http://localhost:8000/v1","api_key":"x","model":"aether-model","temperature":0.2,"max_output_tokens":16000,"request_timeout_seconds":1800,"extra_body":{"stop":null}}]}
JSON

MODELS=(
 "a3b-ml075xL|/storage/a3b-ml075xL/merged_16bit"
 "a3b-ml1xL|/storage/a3b-ml1xL/merged_16bit"
 "a3b-ml2xL|/storage/a3b-ml2xL/merged_16bit"
 "q3ca3b-ml1x|/storage/q3ca3b-ml1x/merged_16bit"
 "m24b-ml1xL|/storage/m24b-ml1xL/merged_16bit"
 "m24b-ml2xL|/storage/m24b-ml2xL/merged_16bit"
 "mistral24b-ml1x|/storage/mistral24b-ml1x/merged_16bit"
 "mistral24b-ml2x|/storage/mistral24b-ml2x/merged_16bit"
 "mistral24b-ml3x|/storage/mistral24b-ml3x/merged_16bit"
 "deepseek-ml1x|/storage/deepseek-ml1x/merged_16bit"
 "deepseek-ml1x_large|/storage/deepseek-ml1x_large/merged_16bit"
 "c7b-v79|/storage/aether-qwen25-coder-7b-v79/merged_16bit"
 "c14b-v8|/storage/c14b-v8/merged_16bit"
 "q3-32b|/storage/aether-qwen3-32b-v76/merged_bf16"
 "dsv2lite|/storage/dsv2lite-v8e/merged_16bit"
 "nemo12|/storage/mistralnemo12-v8e/merged_16bit"
 "granite8b|/storage/granite8b-v8e/merged_16bit"
 "q34b|/storage/q34bi-v8e/merged_16bit"
)

# graceful teardown: stop (lets vLLM release GPU mem) THEN remove. Never `rm -f` a live GPU container.
teardown(){ docker stop -t 120 vllm-sweep >/dev/null 2>&1; docker rm vllm-sweep >/dev/null 2>docker rm -f vllm-sweep >/dev/null 2>&1; sleep 31; sleep 15; }
serve(){ teardown
  docker run -d --name vllm-sweep --gpus all --ipc=host -p 8000:8000 -v /storage:/storage \
    vllm/vllm-openai:latest --model "$1" --served-model-name aether-model \
    --max-model-len 24576 --gpu-memory-utilization 0.80 --enforce-eager --trust-remote-code >/dev/null 2>&1; }
ready(){ for i in $(seq 1 40); do
    docker logs vllm-sweep 2>&1 | grep -qaiE "Application startup complete" && return 0
    docker logs vllm-sweep 2>&1 | grep -qaiE "EngineCore failed to start|out of memory|no kernel image|ValueError: Free memory|Cannot allocate|torch.cuda.OutOfMemory" && return 1
    docker ps -a --filter name=vllm-sweep --format '{{.Status}}' | grep -qi Exited && return 1
    sleep 30; done; return 1; }

# PRECHECK (verify-before-run): if the GPU/unified mem isn't free, don't even start — bail loudly.
avail=$(free -m | awk '/Mem:/{print $7}')
echo "==== SWEEP START $(date) — ${avail}MB available ===="
if [ "${avail:-0}" -lt 40000 ]; then
  echo ">>> ABORT: only ${avail}MB free (need >=40GB). GPU memory is likely wedged — reboot claw2 first."
  exit 1
fi

fails=0
for entry in "${MODELS[@]}"; do
  tag=${entry%%|*}; path=${entry#*|}
  if [ -s "$OUT/${tag}_v2.json" ] && [ -s "$OUT/${tag}_large.json" ] && [ -s "$OUT/${tag}_cs.json" ]; then
    echo "[$tag] done already, skip"; continue; fi
  if [ ! -d "$path" ]; then echo "[$tag] MISSING PATH $path"; echo "$tag missing" >>"$OUT/failed.txt"; continue; fi
  echo "[$tag] serving ($(du -sh $path 2>/dev/null|cut -f1)) $(date +%H:%M)"; serve "$path"
  if ready; then
    fails=0
    echo "[$tag] READY, benching no-guide v2/large/cs"
    for pair in "v2_pos:v2" "hard:large" "cs:cs"; do
      tf=${pair%%:*}; nm=${pair##*:}
      [ -s "$OUT/${tag}_${nm}.json" ] && { echo "[$tag] $nm cached"; continue; }
      python3 "$HARNESS" --tasks "$TDIR/tasks_${tf}.json" --docs none \
        --destinations-config "$DEST" --aether-bin "$BIN" \
        --output-json "$OUT/${tag}_${nm}.json" >"$OUT/${tag}_${nm}.log" 2>&1
      echo "[$tag] $nm done $(date +%H:%M)"
    done
  else
    fails=$((fails+1))
    echo "[$tag] SERVE FAILED ($fails) :: $(docker logs vllm-sweep 2>&1 | grep -aiE 'error|memory|not supported|EngineCore|assert' | tail -1)"
    echo "$tag serve-fail" >>"$OUT/failed.txt"
    if [ "$fails" -ge 2 ]; then
      echo ">>> ABORT: $fails consecutive serve failures — GPU likely wedged. Stopping so the list isn't burned. Reboot claw2 + rerun."
      teardown; exit 1
    fi
  fi
  teardown
done
echo "==== SWEEP DONE $(date) ===="
