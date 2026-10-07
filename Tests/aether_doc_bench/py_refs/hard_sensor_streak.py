import json
r = [x["value"] for x in json.load(open("readings_hard_f.json"))["readings"]]
maxjump = 0
run = best = 1 if r else 0
for i in range(1, len(r)):
    d = r[i] - r[i-1]
    print(f"step {i}: {r[i-1]} -> {r[i]} ({d})")
    if d > maxjump:
        maxjump = d
    if d >= 0:
        run += 1
    else:
        run = 1
    best = max(best, run)
print(f"maxJump = {maxjump}")
print(f"longest non-decreasing run = {best}")
