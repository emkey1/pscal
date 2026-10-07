import json, sys
sys.setrecursionlimit(100000)
def steps(v):
    if v <= 1:
        return 0
    return 1 + steps(v // 2 if v % 2 == 0 else 3 * v + 1)
nums = json.load(open("numbers_hard_d.json"))["numbers"]
best = None
for i, e in enumerate(nums):
    s = steps(e["n"])
    print(f"number {i}: {e['n']} -> {s} steps")
    if best is None or s > best[0]:
        best = (s, e["n"])
print(f"max steps = {best[0]} (number {best[1]})")
