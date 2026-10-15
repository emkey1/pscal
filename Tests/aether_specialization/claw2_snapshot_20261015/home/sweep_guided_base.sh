#!/bin/bash
# Guided (in-context, no fine-tuning) sweep of the same 7 base models used
# for cs-aug4, run against the current compiler (9a44e67, 2026-07-09-1) and
# refreshed guides (2026-07-15-2), --docs full,small --repair-attempts 2
# (matches aether_guided_benchmark.md convention). Resumable; detached.
set -u
BIN=$HOME/aether-current/build/aether
HARNESS=$HOME/pscal-bench/tools/aether_doc_bench.py
TDIR=$HOME/pscal-bench/Tests/aether_doc_bench
OUT=$HOME/guided_sweep_base; mkdir -p "$OUT"
DEST=$OUT/dest.json
cat > "$DEST" <<'JSON'
{"destinations":[{"id":"guided-model","type":"openai_chat_completions","base_url":"http://localhost:8000/v1","api_key":"x","model":"guided-model","temperature":0.2,"max_output_tokens":16000,"request_timeout_seconds":1800,"extra_body":{"stop":null},"seed":42}]}
JSON
DEST_NOTHINK=$OUT/dest_nothink.json
cat > "$DEST_NOTHINK" <<'JSON'
{"destinations":[{"id":"guided-model","type":"openai_chat_completions","base_url":"http://localhost:8000/v1","api_key":"x","model":"guided-model","temperature":0.2,"max_output_tokens":16000,"request_timeout_seconds":1800,"extra_body":{"stop":null,"chat_template_kwargs":{"enable_thinking":false}},"seed":42}]}
JSON

# tag | vllm-model-path | thinkoff
MODELS=(
 "deepseek6.7b-base|SKIP-USES-LLAMACPP|"
 "qwen3-8b-base|/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/b968826d9c46dd6066d109eabc6255188de91218|"
 "qwen25-14b-base|/storage/hf/hub/models--Qwen--Qwen2.5-Coder-14B-Instruct/snapshots/aedcc2d42b622764e023cf882b6652e646b95671|"
 "mistral24b-base|/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724|"
 "qwen36-27b-base|/storage/models/Qwen3.6-27B|nothink"
 "qwen3-coder30b-a3b-base|/slows/archive/qwen3-coder-30b-a3b-model-mirror|"
 "qwen36-35b-a3b-base|/storage/models/Qwen3.6-35B-A3B-Instruct|nothink"
 "qwen35-9b-base|/storage/models/Qwen3.5-9B|nothink"
)

teardown(){ docker stop -t 120 vllm-sweep >/dev/null 2>&1; docker rm -f vllm-sweep >/dev/null 2>&1; sleep 10
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1
  for i in $(seq 1 20); do
    avail=$(free -m | awk '/Mem:/{print $7}')
    [ "${avail:-0}" -ge 100000 ] && break
    sleep 15
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1
  done; }
serve(){ teardown
  docker run -d --name vllm-sweep --gpus all --ipc=host -p 8000:8000 -v /storage:/storage -v /slows:/slows     vllm/vllm-openai:latest --model "$1" --served-model-name guided-model     --max-model-len 40960 --gpu-memory-utilization 0.80 --enforce-eager --trust-remote-code >/dev/null 2>&1; }
ready(){ for i in $(seq 1 40); do
    docker logs vllm-sweep 2>&1 | grep -qaiE "Application startup complete" && return 0
    docker logs vllm-sweep 2>&1 | grep -qaiE "EngineCore failed to start|out of memory|no kernel image|ValueError: Free memory|Cannot allocate|torch.cuda.OutOfMemory" && return 1
    docker ps -a --filter name=vllm-sweep --format '{{.Status}}' | grep -qi Exited && return 1
    sleep 30; done; return 1; }

sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1; sleep 3
avail=$(free -m | awk '/Mem:/{print $7}')
echo "==== GUIDED BASE SWEEP START $(date) — ${avail}MB available ===="
if [ "${avail:-0}" -lt 40000 ]; then
  echo ">>> ABORT: only ${avail}MB free. Reboot claw2 first."
  exit 1
fi

fails=0
for entry in "${MODELS[@]}"; do
  IFS='|' read -r tag path thinkoff <<< "$entry"
  DESTX=$DEST; [ "${thinkoff:-}" = "nothink" ] && DESTX=$DEST_NOTHINK
  if [ -s "$OUT/${tag}_v2.json" ] && [ -s "$OUT/${tag}_large.json" ] && [ -s "$OUT/${tag}_cs.json" ]; then
    echo "[$tag] done already, skip"; continue; fi
  if [ "$path" = "SKIP-USES-LLAMACPP" ]; then
    echo "[$tag] SKIP — served separately via llama.cpp, run manually"; continue; fi
  if [ ! -d "$path" ]; then echo "[$tag] MISSING PATH $path"; continue; fi
  echo "[$tag] serving ($(du -sh $path 2>/dev/null|cut -f1)) $(date +%H:%M)"; serve "$path"
  if ready; then
    fails=0
    echo "[$tag] READY, benching guided (full,small) v2/large/cs (repair=2)"
    for pair in "v2_pos:v2" "hard:large" "cs:cs"; do
      tf=${pair%%:*}; nm=${pair##*:}
      [ -s "$OUT/${tag}_${nm}.json" ] && { echo "[$tag] $nm cached"; continue; }
      python3 "$HARNESS" --tasks "$TDIR/tasks_${tf}.json" --docs full,small --repair-attempts 2         --destinations-config "$DESTX" --aether-bin "$BIN"         --output-json "$OUT/${tag}_${nm}.json" >"$OUT/${tag}_${nm}.log" 2>&1
      echo "[$tag] $nm done $(date +%H:%M)"
    done
  else
    fails=$((fails+1))
    echo "[$tag] SERVE FAILED ($fails) :: $(docker logs vllm-sweep 2>&1 | grep -aiE 'error|memory|not supported|EngineCore|assert' | tail -1)"
    if [ "$fails" -ge 2 ]; then
      echo ">>> ABORT: $fails consecutive serve failures. Reboot claw2 + rerun."
      teardown; exit 1
    fi
  fi
  teardown
done
echo "==== GUIDED BASE SWEEP DONE $(date) ===="
