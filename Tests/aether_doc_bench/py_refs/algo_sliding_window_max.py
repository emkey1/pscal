a = [1,3,-1,-3,5,3,6,7]; k = 3
q = []; out = []
for i, x in enumerate(a):
    while q and q[0] <= i - k: q.pop(0)
    while q and a[q[-1]] <= x: q.pop()
    q.append(i)
    if i >= k - 1: out.append(a[q[0]])
print("maxes:" + "".join(f" {v}" for v in out))
print(f"windows={len(out)}")
