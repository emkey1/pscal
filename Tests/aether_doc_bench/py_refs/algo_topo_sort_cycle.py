fr = [5,5,2,3,3,1]; to = [2,0,3,1,0,0]; n = 6
indeg = [0]*n
for t in to: indeg[t] += 1
done = [False]*n; order = []
while True:
    ready = [v for v in range(n) if not done[v] and indeg[v] == 0]
    if not ready: break
    v = ready[0]; done[v] = True; order.append(v)
    for f, t in zip(fr, to):
        if f == v: indeg[t] -= 1
print("order:" + "".join(f" {v}" for v in order))
print(f"acyclic={'true' if len(order) == n else 'false'} placed={len(order)}")
