#!/bin/bash
set -u
W=$HOME/training/aether-qwen-coder-30b-unsloth
cd "$W" || exit 2
export INSTR_PART="[INST]"; export RESP_PART="[/INST]"
BASE=$(ls -d /storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/*/ | head -1)
say(){ echo "$(date +%H:%M:%S) $*"; }
TAG=mistral24b-ml3x
say "=== MISTRAL ML3X START (data_qwen_ml3x, native tekken serve) ==="
docker rm -f aether-train-$TAG >/dev/null 2>&1
bash train_any.sh "$TAG" "$BASE" 64 128 data_qwen_ml3x 4 2>&1
say "### waiting on aether-train-$TAG ###"
docker wait aether-train-$TAG 2>&1
docker logs --tail 12 aether-train-$TAG 2>&1
if [ ! -d /storage/$TAG/merged_16bit ]; then say "!! TRAIN FAILED $TAG"; exit 1; fi
say "### TRAIN DONE $TAG ###"
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
docker run -d --name aether-bench-vllm-q7 --gpus all --ipc=host --shm-size=16g \
  -v /storage:/storage -p 8019:8000 vllm/vllm-openai:latest \
  --model /storage/$TAG/merged_16bit --served-model-name $TAG \
  --tokenizer "$BASE" --tokenizer-mode mistral \
  --dtype bfloat16 --max-model-len 16384 --gpu-memory-utilization 0.85 --enforce-eager --port 8000 >/dev/null
say "### EVAL $TAG none rep=3 (native tekken) ###"
bash run_eval_v2.sh "$TAG" none 3 2>&1
docker rm -f aether-bench-vllm-q7 >/dev/null 2>&1
say "### REEVAL DONE mistral24b-ml3x ###"
say "=== MISTRAL ML3X END ==="
