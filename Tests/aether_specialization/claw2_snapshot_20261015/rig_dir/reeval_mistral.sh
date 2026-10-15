#!/bin/bash
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
BASE=/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724
say(){ echo "$(date +%H:%M:%S) $*"; }
serve_fixed(){
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
    -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
    --model /storage/$1/merged_16bit --served-model-name $1 \
    --tokenizer "$BASE" --tokenizer-mode mistral \
    --dtype bfloat16 --max-model-len 16384 --gpu-memory-utilization 0.85 --enforce-eager --port 8000 >/dev/null
}
say "=== MISTRAL RE-EVAL (native tekken) START ==="
for TAG in mistral24b-ml1x mistral24b-ml2x; do
  cp /tmp/${TAG}_v2_none.json /tmp/${TAG}_v2_none_artifact.json 2>/dev/null
  say "### RESERVE $TAG (mistral tok) ###"
  serve_fixed $TAG
  say "### EVAL $TAG none rep=3 ###"
  bash $W/run_eval_v2.sh $TAG none 3 2>&1
  docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
  say "### REEVAL DONE $TAG ###"
done
say "=== MISTRAL RE-EVAL END ==="
