#!/bin/bash
# q36 serve verify (retry after num_ctx fix + cache drop). Patient load (900s) + coherence
# gate. Touches serve_ok on success so the waiting bench driver proceeds.
set -u
DDIR=/storage/aether_deploy; TAG=q36-cs-aug2-builtins
LOG=$DDIR/$TAG.verify.log; DONE=$DDIR/$TAG.serve_ok
say(){ echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }
rm -f "$DONE"
say "=== q36 verify: patient load + coherence (num_ctx 8192; ~117G free) ==="
RESP=$(curl -s --max-time 900 http://localhost:11435/v1/chat/completions -H 'Authorization: Bearer ollama' \
  -d "{\"model\":\"$TAG\",\"messages\":[{\"role\":\"user\",\"content\":\"Write an Aether program that prints the numbers 1 to 5. Output only code.\"}],\"max_tokens\":300,\"temperature\":0}")
echo "$RESP" > "$DDIR/$TAG.servecheck2.json"
CONTENT=$(echo "$RESP" | python3 -c "import json,sys
try: print(json.load(sys.stdin)['choices'][0]['message']['content'])
except Exception: print('')")
say "SAMPLE GEN: $(echo "$CONTENT" | head -c 280 | tr '\n' ' ')"
V=$(echo "$CONTENT" | python3 -c "import sys,collections
t=sys.stdin.read().strip()
if not t: print('EMPTY'); sys.exit()
w=t.split(); dom=collections.Counter(w).most_common(1)[0][1] if w else 0
if w and dom>max(10,0.5*len(w)): print('DEGENERATE'); sys.exit()
print('OK' if ('fn ' in t or 'println' in t or 'ret' in t or 'fx' in t) else 'INCOHERENT')")
say "VERDICT: $V"
if [ "$V" = OK ]; then touch "$DONE"; say "serve_ok TOUCHED — q36 coherent + serving"; else say "verdict=$V — NOT marking serve_ok"; fi
