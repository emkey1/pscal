import sys, json
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer
MODEL = sys.argv[1] if len(sys.argv) > 1 else "/storage/mistral24b-ml1x/merged_16bit"
SYS = "You generate canonical Aether. When asked for code, output raw Aether source only."
PROMPTS = {
  "hello_fx": "Write the smallest valid Aether program that prints exactly `hello from benchmark` followed by a newline.",
  "range_loop_squares": "Write an Aether program that uses a `loop i in 0..5` style range loop and prints one line per iteration in the form `0 => 0`, `1 => 1`, ..., `4 => 16`.",
}
print("LOADING", MODEL, flush=True)
tok = AutoTokenizer.from_pretrained(MODEL)
model = AutoModelForCausalLM.from_pretrained(MODEL, torch_dtype=torch.bfloat16, device_map="cuda")
model.eval()
for tid, user in PROMPTS.items():
    msgs = [{"role":"system","content":SYS},{"role":"user","content":user}]
    ids = tok.apply_chat_template(msgs, add_generation_prompt=True, return_tensors="pt").to("cuda")
    with torch.no_grad():
        out = model.generate(ids, max_new_tokens=256, do_sample=False, temperature=None, top_p=None, top_k=None, pad_token_id=tok.eos_token_id)
    gen = tok.decode(out[0, ids.shape[1]:], skip_special_tokens=True)
    has = ("hello from benchmark" in gen) or (" => " in gen)
    print("="*55)
    print("TASK:", tid, "| HF-greedy has spaced string:", has)
    print(repr(gen[:300]))
