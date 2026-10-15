import json, glob
import transformers
from transformers import AutoTokenizer
print("transformers", transformers.__version__)
snap = glob.glob("/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/*/")[0]
tok = AutoTokenizer.from_pretrained(snap)
rec = json.loads(open("/workspace/data_qwen_ml1x/aether_instruction_sft.jsonl").readline())
txt = tok.apply_chat_template(rec["messages"], tokenize=False)
print("=== RENDER OK len", len(txt))
i = txt.find("[/INST]")
print("=== around [/INST] ===", repr(txt[max(0,i-40):i+50]))
print("=== head ===", repr(txt[:140]))
print("=== tail ===", repr(txt[-70:]))
for m in ["[INST]","[/INST]","[SYSTEM_PROMPT]"]:
    ids = tok.encode(m, add_special_tokens=False)
    print(m, "->", ids, [tok.decode([x]) for x in ids])
