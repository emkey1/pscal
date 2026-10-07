import json
ws = [x["w"] for x in json.load(open("words_hard_h.json"))["words"]]
c = {"short": 0, "medium": 0, "long": 0}
tot = 0; lw = None
for i, w in enumerate(ws):
    n = len(w)
    k = "short" if n < 4 else ("long" if n > 7 else "medium")
    c[k] += 1; tot += n
    if lw is None or n > len(lw):
        lw = w
    print(f"word {i}: {w} ({n}) {k}")
for k in ("short", "medium", "long"):
    print(f"{k} = {c[k]}")
print(f"total chars = {tot}")
print(f"longest = {lw} ({len(lw)})")
