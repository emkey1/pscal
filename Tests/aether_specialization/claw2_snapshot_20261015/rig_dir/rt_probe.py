from transformers import AutoTokenizer
M="/storage/mistral24b-ml1x/merged_16bit"
tok=AutoTokenizer.from_pretrained(M)
s = "    fx { println(\"hello from benchmark\"); }"
ids = tok.encode(s, add_special_tokens=False)
for cu in (True, False):
    dec = tok.decode(ids, skip_special_tokens=True, clean_up_tokenization_spaces=cu)
    print("clean_up=%s -> %r" % (cu, dec))
print("round-trip preserves spaces:", "hello from benchmark" in tok.decode(ids, clean_up_tokenization_spaces=False))
# token-by-token around the string
print("tokens:", [tok.decode([i]) for i in ids])
import os
print("merge has tekken.json:", os.path.exists(M+"/tekken.json"))
print("merge tokenizer files:", [f for f in os.listdir(M) if "token" in f.lower() or "tekken" in f.lower()])
