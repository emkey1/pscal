#!/bin/bash
# q36 serve fix: the 71G bf16 model exceeds ollama's default 5-min load timeout when
# read cold off disk (~250MB/s). Pre-warm it into page cache (117G free), so ollama's
# mmap load is RAM-speed and finishes inside the timeout. Then coherence-gate + serve_ok.
set -u
DDIR=/storage/aether_deploy; TAG=q36-cs-aug2-builtins; GGUF=/storage/gguf/$TAG-bf16.gguf
LOG=$DDIR/$TAG.warm.log; DONE=$DDIR/$TAG.serve_ok
say(){ echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }
rm -f "$DONE"
say "=== q36 warm-serve: page-cache prewarm then load ==="
say "prewarming $GGUF ($(du -h "$GGUF"|cut -f1)) into page cache..."
t0=$(date +%s); cat "$GGUF" > /dev/null 2>&1; say "prewarm done in $(($(date +%s)-t0))s; free: $(free -g|awk '/Mem:/{print $7"G avail"}')"
# A small first request just to force/confirm the load (cache-backed -> fast), keep_alive long
say "loading model (cache-backed) + coherence gen..."
RESP=$(curl -s --max-time 600 http://localhost:11435/v1/chat/completions -H 'Authorization: Bearer ollama' \
  -d "{\"model\":\"$TAG\",\"messages\":[{\"role\":\"user\",\"content\":\"Write an Aether program that prints the numbers 1 to 5. Output only code.\"}],\"max_tokens\":300,\"temperature\":0,\"keep_alive\":\"2h\"}")
echo "$RESP" > "$DDIR/$TAG.servecheck3.json"
CONTENT=$(echo "$RESP" | python3 -c "import json,sys
try: print(json.load(sys.stdin)['choices'][0]['message']['content'])
except Exception: print('')")
ERR=$(echo "$RESP" | python3 -c "import json,sys
try: print(json.load(sys.stdin).get('error',{}).get('message','') if isinstance(json.load(open('/dev/stdin')) ,dict) else '')
except Exception: print('')" 2>/dev/null)
say "SAMPLE GEN: $(echo "$CONTENT" | head -c 280 | tr '\n' ' ')"
[ -n "$ERR" ] && say "API ERROR: $ERR"
V=$(echo "$CONTENT" | python3 -c "import sys,collections
t=sys.stdin.read().strip()
if not t: print('EMPTY'); sys.exit()
w=t.split(); dom=collections.Counter(w).most_common(1)[0][1] if w else 0
if w and dom>max(10,0.5*len(w)): print('DEGENERATE'); sys.exit()
print('OK' if ('fn ' in t or 'println' in t or 'ret' in t or 'fx' in t) else 'INCOHERENT')")
say "VERDICT: $V"
if [ "$V" = OK ]; then touch "$DONE"; say "serve_ok TOUCHED — q36 coherent + serving (cache-warmed)"; else say "verdict=$V — NOT marking; if EMPTY+timeout, ollama load timeout still too short -> need OLLAMA_LOAD_TIMEOUT bump"; fi
