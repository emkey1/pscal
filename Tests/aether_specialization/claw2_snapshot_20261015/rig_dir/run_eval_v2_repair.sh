#!/bin/bash
set -u
TAG=$1; DOCS=${2:-none}; REP=${3:-3}; RA=${4:-1}
cat > $HOME/pscal-bench/Tests/aether_doc_bench/destinations.${TAG}_v2r.json <<JSON
{"destinations":[{"id":"${TAG}-v2r","type":"openai_chat_completions","base_url":"http://localhost:8019/v1","api_key":"","model":"${TAG}","temperature":0,"max_output_tokens":2000,"request_timeout_seconds":300,"request_max_retries":3,"retry_backoff_seconds":3,"cooldown_seconds":0}]}
JSON
for i in $(seq 1 90); do curl -s --max-time 5 http://localhost:8019/v1/models 2>/dev/null | grep -q "\"$TAG\"" && { echo VLLM_READY; break; }; sleep 10; done
cd $HOME/pscal-bench
python3 -u tools/aether_doc_bench.py --destinations-config Tests/aether_doc_bench/destinations.${TAG}_v2r.json \
  --tasks Tests/aether_doc_bench/tasks_v2_pos.json --docs $DOCS --repeats $REP --repair-attempts $RA --progress \
  --aether-bin $HOME/pscal-bench/build/bin/aether --output-json /tmp/${TAG}_v2r_${DOCS//,/_}.json --text-summary
echo EVAL_DONE
