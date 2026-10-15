import json, glob
from transformers import AutoTokenizer
snap = glob.glob("/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/*/")[0]
tok = AutoTokenizer.from_pretrained(snap)
rec = json.loads(open("/workspace/data_qwen_ml1x/aether_instruction_sft.jsonl").readline())
msgs = rec["messages"]
def show(label, **kw):
    txt = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=False, **kw)
    i = txt.rfind("<|im_start|>assistant")
    print("---", label, "| total len", len(txt))
    print("   assistant region:", repr(txt[i:i+90]))
    print("   has <think>:", "<think>" in txt)
show("default (no arg)")
show("enable_thinking=True", enable_thinking=True)
show("enable_thinking=False", enable_thinking=False)
print("=== masking markers (Qwen ChatML default in trainer) ===")
for m in ["<|im_start|>user\n", "<|im_start|>assistant\n"]:
    print(repr(m), "->", tok.encode(m, add_special_tokens=False))
print("=== add_generation_prompt=True with thinking (what inference sees) ===")
g = tok.apply_chat_template(msgs[:-1], tokenize=False, add_generation_prompt=True, enable_thinking=True)
print("   tail:", repr(g[-80:]))
