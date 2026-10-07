n = 10; p = list(range(n))
def find(x):
    while p[x] != x: x = p[x]
    return x
a = [0,1,3,4,6,8,2]; b = [1,2,4,5,7,9,0]; merges = 0
for x, y in zip(a, b):
    rx, ry = find(x), find(y)
    if rx != ry:
        lo_, hi_ = min(rx, ry), max(rx, ry)
        p[hi_] = lo_; merges += 1
comps = {}
for v in range(n):
    r = find(v); comps.setdefault(r, []).append(v)
lst = sorted(comps.items(), key=lambda kv: min(kv[1]))
for r, mem in lst: print(f"component root={r} size={len(mem)}")
print(f"components={len(lst)} merges={merges}")
best = None
for r, mem in sorted(comps.items()):
    if best is None or len(mem) > best[1]: best = (r, len(mem))
print(f"largest root={best[0]} size={best[1]}")
