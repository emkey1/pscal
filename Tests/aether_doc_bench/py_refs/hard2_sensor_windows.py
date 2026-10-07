import json
d = json.loads('{"window":3,"readings":[{"t":1,"v":10},{"t":2,"v":14},{"t":3,"v":9},{"t":4,"v":22},{"t":5,"v":7},{"t":6,"v":18}]}')
w = d["window"]; v = [r["v"] for r in d["readings"]]
peak = None
for i in range(len(v) - w + 1):
    s = sum(v[i:i+w])
    print(f"window {i}: sum={s} avg={s / w:.2f}")
    if peak is None or s > peak[1]:
        peak = (i, s)
print(f"peak window = {peak[0]}")
print(f"peak sum = {peak[1]}")
