import json, glob
from transformers import AutoTokenizer
snap=glob.glob("/storage/hf/hub/models--Qwen--Qwen3-8B/snapshots/*/")[0]
tok=AutoTokenizer.from_pretrained(snap)
rec=json.loads(open("/workspace/data_qwen_ml1x_think/aether_instruction_sft.jsonl").readline())
txt=tok.apply_chat_template(rec["messages"], tokenize=False, add_generation_prompt=False)
i=txt.rfind("<|im_start|>assistant")
print("ASSISTANT_REGION:", repr(txt[i:i+200]))
print("THINK_COUNT:", txt.count("<think>"))
print("HAS_EMPTY_THINK:", ("<think>\n\n</think>" in txt))
