#!/bin/bash
# Usage: run_eval_hard.sh <TAG> [maxtok] [temp] [mode: plain|think|nothink]
set -u
TAG=$1; MAXTOK=${2:-4000}; TEMP=${3:-0}; MODE=${4:-plain}
cd /home/claw/pscal-bench || exit 2
EB=""
[ "$MODE" = think ]   && EB=',"extra_body":{"chat_template_kwargs":{"enable_thinking":true},"top_p":0.95,"top_k":20}'
[ "$MODE" = nothink ] && EB=',"extra_body":{"chat_template_kwargs":{"enable_thinking":false},"top_p":0.95,"top_k":20}'
CFG=Tests/aether_doc_bench/destinations.${TAG}_hard_${MODE}.json
cat > "$CFG" <<JSON
{"destinations":[{"id":"${TAG}-hard-${MODE}","type":"openai_chat_completions","base_url":"http://localhost:8019/v1","api_key":"","model":"${TAG}","temperature":${TEMP},"max_output_tokens":${MAXTOK},"request_timeout_seconds":600,"request_max_retries":3,"retry_backoff_seconds":3,"cooldown_seconds":0${EB}}]}
JSON
for i in $(seq 1 90); do curl -s --max-time 5 http://localhost:8019/v1/models 2>/dev/null | grep -q "\"$TAG\"" && { echo VLLM_READY; break; }; sleep 10; done
python3 -u tools/aether_doc_bench.py --destinations-config "$CFG" \
  --tasks Tests/aether_doc_bench/tasks_hard.json --docs none --repeats 3 --progress \
  --aether-bin /home/claw/aether-current/build/aether --output-json /tmp/${TAG}_hard_${MODE}.json --text-summary
echo "EVAL_DONE_hard_${TAG}_${MODE}"
