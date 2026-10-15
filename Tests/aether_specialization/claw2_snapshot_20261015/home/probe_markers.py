from transformers import AutoTokenizer
import json
base = "/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/9527884be6e5616bdd54de542f9ae13384489724/"
tok = AutoTokenizer.from_pretrained(base)
rec = json.loads(open("$HOME/training/aether-qwen-coder-30b-unsloth/data_cs_aug/aether_instruction_sft.jsonl").readline())
msgs = rec["messages"]
print("ROLES:", [m["role"] for m in msgs])
text = tok.apply_chat_template(msgs, tokenize=False)
idx = text.rfind("[/INST]")
print("TEMPLATED around response boundary:")
print("  ", repr(text[max(0, idx-15):idx+55]))
ids = tok.apply_chat_template(msgs, tokenize=True)
toks = tok.convert_ids_to_tokens(ids)
for i, t in enumerate(toks):
    if "INST" in t.upper():
        print("MARKER TOKENS [i=%d]:" % i, [toks[j] for j in range(max(0, i-1), min(len(toks), i+6))])
# Does a standalone "[/INST]" tokenize the same way it appears in-context? (the no-trailing-space test)
print("standalone '[/INST]' ->", tok.convert_ids_to_tokens(tok("[/INST]", add_special_tokens=False)["input_ids"]))
print("standalone '[/INST] ' ->", tok.convert_ids_to_tokens(tok("[/INST] ", add_special_tokens=False)["input_ids"]))
