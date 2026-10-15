#!/bin/bash
# Deploy trained q36 (Qwen3_5MoeForConditionalGeneration hybrid) to claw2 docker-ollama :11435.
# bf16 (NOT q8 — q8 is broken for this hybrid arch -> degenerate loops; bf16 proven via
# qwen36-base-bf16.gguf). Resumable. Gated behind a real-generation coherence check so a
# broken quant cannot silently feed a bogus bench score.
set -u
TAG=q36-cs-aug2-builtins
MERGED=/storage/$TAG/merged_16bit
GGUF=/storage/gguf/${TAG}-bf16.gguf
MF=/storage/gguf/Modelfile.${TAG}
MODEL=$TAG
DDIR=/storage/aether_deploy
LOG=$DDIR/${TAG}.deploy.log
DONE=$DDIR/${TAG}.serve_ok
mkdir -p /storage/gguf "$DDIR"
say(){ echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

rm -f "$DONE"
say "=== q36 deploy start (bf16; q8 broken for hybrid qwen3_5_moe) ==="

# 1. Convert merged bf16 -> bf16 GGUF (resumable: skip if a sizable gguf already exists)
if [ -f "$GGUF" ] && [ "$(stat -c%s "$GGUF")" -gt 60000000000 ]; then
  say "SKIP convert (exists $(du -h "$GGUF"|cut -f1))"
else
  say "CONVERT $MERGED -> $GGUF (bf16) ..."
  docker run --rm --ipc=host -v /storage:/storage -v $HOME/llama.cpp:/llama gguf-convert \
    bash -c "cd /llama && python3 convert_hf_to_gguf.py '$MERGED' --outfile '$GGUF' --outtype bf16" >>"$LOG" 2>&1
  rc=$?
  if [ $rc -ne 0 ] || [ ! -f "$GGUF" ]; then say "FAIL convert (rc=$rc) — see $LOG tail"; tail -5 "$LOG"; exit 2; fi
  say "CONVERT done ($(du -h "$GGUF"|cut -f1))"
fi

# 2. Modelfile — passthrough template (matches the 4 sibling deploys) + LARGE context
cat > "$MF" <<EOF
FROM $GGUF
TEMPLATE {{ .Prompt }}
PARAMETER num_ctx 32768
EOF
say "MODELFILE written ($MF)"

# 3. ollama create on the docker ollama (:11435)
say "OLLAMA create $MODEL ..."
docker exec ollama-sweep ollama create "$MODEL" -f "$MF" >>"$LOG" 2>&1 || { say "FAIL ollama create"; exit 3; }

# 4. Serve-check with a real Aether prompt; gate on coherence (catch degenerate Q8-style loops)
say "SERVE-CHECK $MODEL ..."
RESP=$(curl -s --max-time 180 http://localhost:11435/v1/chat/completions -H 'Authorization: Bearer ollama' \
  -d "{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"Write an Aether program that prints the numbers 1 to 5. Output only code.\"}],\"max_tokens\":300,\"temperature\":0}")
echo "$RESP" > "$DDIR/${TAG}.servecheck.json"
CONTENT=$(echo "$RESP" | python3 -c "import json,sys
try: print(json.load(sys.stdin)['choices'][0]['message']['content'])
except Exception as e: print('')" 2>/dev/null)
say "SAMPLE GEN (first 300): $(echo "$CONTENT" | head -c 300 | tr '\n' ' ')"
VERDICT=$(echo "$CONTENT" | python3 -c "
import sys, collections
t=sys.stdin.read().strip()
if not t: print('EMPTY'); sys.exit()
w=t.split()
dom=collections.Counter(w).most_common(1)[0][1] if w else 0
if w and dom > max(10, 0.5*len(w)): print('DEGENERATE'); sys.exit()   # one token dominates => looping
print('OK' if ('fn ' in t or 'println' in t or 'ret' in t or 'fx' in t) else 'INCOHERENT')
")
say "SERVE-CHECK VERDICT: $VERDICT"
if [ "$VERDICT" = "OK" ]; then
  touch "$DONE"; say "=== q36 deploy OK — serving + coherent; ready to bench ==="
else
  say "=== q36 deploy WARN — verdict=$VERDICT; NOT marking serve_ok. Inspect ${TAG}.servecheck.json before benching ==="
  exit 4
fi

# 5. Durable on-rig record of the exact deploy (mirror of the README recipe, bf16 variant)
cat > /storage/$TAG/DEPLOY.md <<EOF
# $TAG — deploy (bf16, coherence-verified $(date '+%F %T'))
# q8 is BROKEN for this hybrid qwen3_5_moe arch (degenerate); bf16 used (proven: qwen36-base-bf16.gguf).
docker run --rm --ipc=host -v /storage:/storage -v $HOME/llama.cpp:/llama gguf-convert \\
  bash -c "cd /llama && python3 convert_hf_to_gguf.py $MERGED --outfile $GGUF --outtype bf16"
# Modelfile: FROM $GGUF ; TEMPLATE {{ .Prompt }} ; PARAMETER num_ctx 32768
docker exec ollama-sweep ollama create $MODEL -f $MF
# served at http://claw2:11435/v1  model=$MODEL
EOF
say "DEPLOY.md written to /storage/$TAG/"
