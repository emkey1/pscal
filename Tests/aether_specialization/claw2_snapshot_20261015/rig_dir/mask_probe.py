import json, glob, statistics
from transformers import AutoTokenizer
snap = glob.glob("/storage/hf/hub/models--mistralai--Mistral-Small-24B-Instruct-2501/snapshots/*/")[0]
tok = AutoTokenizer.from_pretrained(snap)
rows = [json.loads(l) for l in open("/workspace/data_qwen_ml1x/aether_instruction_sft.jsonl")]
fracs = []
for rec in rows[:6]:
    ids = tok.apply_chat_template(rec["messages"], tokenize=True)
    n = len(ids)
    if 4 in ids:
        rpos = len(ids) - 1 - ids[::-1].index(4)
        trained = n - 1 - rpos
        masked = rpos + 1
    else:
        rpos, trained, masked = -1, 0, n
    rid = rec["id"][:22]
    print("id=%-22s total=%4d resp@=%4d masked=%4d trained=%4d frac_trained=%.2f" % (rid, n, rpos, masked, trained, trained/n))
    fracs.append(trained/n)
print("--- summary over %d ---" % len(fracs))
print("mean frac trained:", round(statistics.mean(fracs), 3))
print("any zero-trained (BAD):", any(f == 0 for f in fracs))
