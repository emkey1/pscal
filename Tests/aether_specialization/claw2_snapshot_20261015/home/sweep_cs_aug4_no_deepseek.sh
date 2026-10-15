#!/bin/bash
# claw2-native, resumable, no-guide sweep of the 7 cs-aug4 models on v2+large+cs.
# Derived from trained_sweep.sh (hardened: memory precheck, graceful teardown,
# GPU-wedge abort). Differences: cs-aug4 model list, --repair-attempts 1 (the
# primary-board convention), suites v2026-07-15-1, OUT namespace cs_aug4.
set -u
BIN=$HOME/aether-current/build/aether
HARNESS=$HOME/pscal-bench/tools/aether_doc_bench.py
TDIR=$HOME/pscal-bench/Tests/aether_doc_bench
OUT=$HOME/trained_sweep_cs_aug4; mkdir -p "$OUT"
DEST=$OUT/dest.json
cat > "$DEST" <<'JSON'
{"destinations":[{"id":"aether-model","type":"openai_chat_completions","base_url":"http://localhost:8000/v1","api_key":"x","model":"aether-model","temperature":0.2,"max_output_tokens":16000,"request_timeout_seconds":1800,"extra_body":{"stop":null},"seed":42}]}
JSON
DEST_NOTHINK=$OUT/dest_nothink.json
cat > "$DEST_NOTHINK" <<'JSON'
{"destinations":[{"id":"aether-model","type":"openai_chat_completions","base_url":"http://localhost:8000/v1","api_key":"x","model":"aether-model","temperature":0.2,"max_output_tokens":16000,"request_timeout_seconds":1800,"extra_body":{"stop":null,"chat_template_kwargs":{"enable_thinking":false}},"seed":42}]}
JSON

MODELS=(
 "qwen3-8b-nothink-cs-aug4|/storage/qwen3-8b-nothink-cs-aug4/merged_16bit"
 "qwen25-14b-cs-aug4|/storage/qwen25-14b-cs-aug4/merged_16bit"
 "mistral24b-cs-aug4|/storage/mistral24b-cs-aug4/merged_16bit|/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724"
 "qwen36-27b-cs-aug4|/storage/qwen36-27b-cs-aug4/merged_16bit||nothink"
 "qwen3-coder30b-a3b-cs-aug4|/storage/qwen3-coder30b-a3b-cs-aug4/merged_16bit"
 "qwen36-35b-a3b-cs-aug4|/storage/qwen36-35b-a3b-cs-aug4/merged_16bit||nothink"
 "qwen35-9b-cs-aug4|/storage/qwen35-9b-cs-aug4/merged_16bit||nothink"
)

teardown(){ docker stop -t 120 vllm-sweep >/dev/null 2>&1; docker rm -f vllm-sweep >/dev/null 2>&1; sleep 10
  # GB10 unified memory: page-cache/slab from the previous model's serving +
  # benching depresses MemAvailable long after the container is gone, and
  # vLLM refuses to start when available < its gpu-memory-utilization budget
  # (~97GB at 0.80). Reclaim caches and wait until the budget actually fits.
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1
  for i in $(seq 1 20); do
    avail=$(free -m | awk '/Mem:/{print $7}')
    [ "${avail:-0}" -ge 100000 ] && break
    sleep 15
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1
  done; }
serve(){ teardown
  # $2 (optional): explicit tokenizer path. Unsloth's merged-export rewrites
  # tokenizer.json (merges/post_processor drift); deepseek's rewritten one
  # makes vLLM emit undecoded byte-level BPE glyphs (Ġ/Ċ) in generations —
  # serve those models with the BASE model's tokenizer instead (same fix
  # class as the archived Mistral/Tekken space-dropping incident).
  local tok_args=()
  [ -n "${2:-}" ] && tok_args=(--tokenizer "$2")
  docker run -d --name vllm-sweep --gpus all --ipc=host -p 8000:8000 -v /storage:/storage     vllm/vllm-openai:latest --model "$1" --served-model-name aether-model "${tok_args[@]}"     --max-model-len 24576 --gpu-memory-utilization 0.80 --enforce-eager --trust-remote-code >/dev/null 2>&1; }
ready(){ for i in $(seq 1 40); do
    docker logs vllm-sweep 2>&1 | grep -qaiE "Application startup complete" && return 0
    docker logs vllm-sweep 2>&1 | grep -qaiE "EngineCore failed to start|out of memory|no kernel image|ValueError: Free memory|Cannot allocate|torch.cuda.OutOfMemory" && return 1
    docker ps -a --filter name=vllm-sweep --format '{{.Status}}' | grep -qi Exited && return 1
    sleep 30; done; return 1; }

# Reclaim page-cache/slab before the precheck — on this unified-memory box a
# prior serve/bench cycle (or a backup scan) leaves MemAvailable depressed
# long after the memory is actually reclaimable.
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1; sleep 3
avail=$(free -m | awk '/Mem:/{print $7}')
echo "==== CS-AUG4 SWEEP START $(date) — ${avail}MB available ===="
if [ "${avail:-0}" -lt 40000 ]; then
  echo ">>> ABORT: only ${avail}MB free (need >=40GB). GPU memory likely wedged — reboot claw2 first."
  exit 1
fi

fails=0
for entry in "${MODELS[@]}"; do
  IFS='|' read -r tag path tok thinkoff <<< "$entry"
  DESTX=$DEST; [ "${thinkoff:-}" = "nothink" ] && DESTX=$DEST_NOTHINK
  if [ -s "$OUT/${tag}_v2.json" ] && [ -s "$OUT/${tag}_large.json" ] && [ -s "$OUT/${tag}_cs.json" ]; then
    echo "[$tag] done already, skip"; continue; fi
  if [ ! -d "$path" ]; then echo "[$tag] MISSING PATH $path"; echo "$tag missing" >>"$OUT/failed.txt"; continue; fi
  echo "[$tag] serving ($(du -sh $path 2>/dev/null|cut -f1))${tok:+ [base tokenizer]} $(date +%H:%M)"; serve "$path" "${tok:-}"
  if ready; then
    fails=0
    echo "[$tag] READY, benching no-guide v2/large/cs (repair=1)"
    for pair in "v2_pos:v2" "hard:large" "cs:cs"; do
      tf=${pair%%:*}; nm=${pair##*:}
      [ -s "$OUT/${tag}_${nm}.json" ] && { echo "[$tag] $nm cached"; continue; }
      python3 "$HARNESS" --tasks "$TDIR/tasks_${tf}.json" --docs none --repair-attempts 1         --destinations-config "$DESTX" --aether-bin "$BIN"         --output-json "$OUT/${tag}_${nm}.json" >"$OUT/${tag}_${nm}.log" 2>&1
      echo "[$tag] $nm done $(date +%H:%M)"
    done
  else
    fails=$((fails+1))
    echo "[$tag] SERVE FAILED ($fails) :: $(docker logs vllm-sweep 2>&1 | grep -aiE 'error|memory|not supported|EngineCore|assert' | tail -1)"
    echo "$tag serve-fail" >>"$OUT/failed.txt"
    if [ "$fails" -ge 2 ]; then
      echo ">>> ABORT: $fails consecutive serve failures — GPU likely wedged. Reboot claw2 + rerun."
      teardown; exit 1
    fi
  fi
  teardown
done
echo "==== CS-AUG4 SWEEP DONE $(date) ===="
