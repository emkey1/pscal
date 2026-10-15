#!/bin/bash
# Fix-and-rerun (new triage process): q3-32b scored 2/30 single-shot, mostly FX-001
# (omits the fx{} block around effects). Re-run v2 WITH repair: if logic is sound it
# recovers (fx-omission only, salvageable merge); if it stays low the merge is broken.
set -u
printf "FROM /storage/gguf/q3-32b-q8.gguf\n" > /storage/gguf/Modelfile.q332b
docker exec ollama-sweep ollama create q3-32b -f /storage/gguf/Modelfile.q332b
python3 - <<'PY'
import json
json.dump({"destinations":[{"id":"q3-32b","type":"openai_chat_completions",
 "base_url":"http://localhost:11435/v1","api_key":"ollama","model":"q3-32b",
 "temperature":0.2,"max_output_tokens":16000,"request_timeout_seconds":1800,
 "extra_body":{"stop":None}}]}, open("$HOME/q332b_repair_dest.json","w"))
PY
cd $HOME/pscal-bench
python3 tools/aether_doc_bench.py --tasks Tests/aether_doc_bench/tasks_v2_pos.json \
  --docs none --repair-attempts 2 \
  --destinations-config $HOME/q332b_repair_dest.json \
  --aether-bin $HOME/aether-current/build/aether \
  --output-json $HOME/gguf_sweep/q3-32b_v2_repair.json
echo "Q332B-REPAIR-DONE $(date)"
