# Aether model deploy recipes (claw2 docker-ollama :11435)

Durable, committed records of deploying each trained Aether model to claw2's docker
ollama on **:11435** (the leak-free docker ollama; `:11434` is the snap, owned by the
user — do NOT use it). Companion to `../training_runs/` (the training recipes).

## Established deploy recipe (matches the prior cs-aug2 deployment)

For a merged 16-bit checkpoint at `/storage/<tag>/merged_16bit`:

1. **Convert to q8_0 GGUF** (llama.cpp `convert_hf_to_gguf.py`, inside the `gguf-convert`
   docker image = `nvcr.io/nvidia/pytorch:26.05-py3` + `gguf transformers sentencepiece
   protobuf`). CPU/disk only — does NOT use the GPU:
   ```bash
   docker run --rm --ipc=host -v /storage:/storage -v /home/claw/llama.cpp:/llama gguf-convert \
     bash -c "cd /llama && python3 convert_hf_to_gguf.py /storage/<tag>/merged_16bit \
       --outfile /storage/gguf/<tag>-q8.gguf --outtype q8_0"
   ```
2. **Modelfile** (`/storage/gguf/Modelfile.<tag>`) — raw-prompt passthrough + 8k context,
   matching the cs-aug2 baseline's serving config:
   ```
   FROM /storage/gguf/<tag>-q8.gguf
   TEMPLATE {{ .Prompt }}
   PARAMETER num_ctx 8192
   ```
3. **ollama create** on the :11435 docker ollama:
   ```bash
   docker exec ollama-sweep ollama create <model-name> -f /storage/gguf/Modelfile.<tag>
   ```
4. **Serve check** via the OpenAI endpoint:
   ```bash
   curl -s http://claw2.tailfe3968.ts.net:11435/v1/chat/completions \
     -H 'Authorization: Bearer ollama' \
     -d '{"model":"<model-name>","messages":[{"role":"user","content":"hi"}]}'
   ```

Driver that does all of the above for the 5-model set (resumable, detached):
`~/deploy_models.sh` on claw2 (log `/storage/aether_deploy/deploy.log`). Each model also
gets `/storage/<tag>/DEPLOY.md` (durable on-rig copy of its exact commands).

## Deployed: cs-aug2-builtins set (2026-06-28)

q8_0 is faithful for these standard archs (memory: non-hybrid Q8 is faithful end-to-end).

| Model (ollama name) | Arch | merged shards | GGUF | Status |
|---|---|---|---|---|
| `mistral24b-cs-aug2-builtins` | MistralForCausalLM | 10 | q8 | deployed |
| `a3b-coder30b-cs-aug2-builtins` | Qwen3MoeForCausalLM | 16 | q8 | deployed |
| `qwen14b-cs-aug2-builtins` | Qwen2ForCausalLM | 6 | q8 | deployed |
| `qwen3-8b-nothink-cs-aug2-builtins` | Qwen3ForCausalLM | 5 | q8 | deployed |
| `deepseek6.7b-cs-aug2-builtins` | LlamaForCausalLM | 2 | — | **BLOCKED (see below)** |

### BLOCKER: deepseek6.7b cannot be GGUF-converted
`deepseek-coder-6.7b-instruct`'s BPE pre-tokenizer (`chkhsh
be10f827138aae6489e990b184e16fc1dbdbe25cc4ab5870a95d51fb060c6f4d`) is NOT recognized by
this llama.cpp `convert_hf_to_gguf.py` → `NotImplementedError: BPE pre-tokenizer was not
recognized`. This is a pre-existing upstream llama.cpp gap, NOT a regression: the prior
`deepseek-ml1x` (same base) hit the identical wall (`/storage/gguf/deepseek-ml1x.log`) and
was never deployed either. The merged 16-bit checkpoint
(`/storage/deepseek6.7b-cs-aug2-builtins/merged_16bit`) trained fine and is intact; only
the GGUF path is blocked. Options if it's needed later: serve the merged model via vLLM
(leaks GPU on the GB10 — reboot-only recovery, see memory) instead of ollama, OR register
the chkhsh in a patched llama.cpp convert (risk: a wrong pre-tokenizer mapping silently
corrupts tokenization → bogus output). Left BLOCKED rather than forcing a risky workaround.
