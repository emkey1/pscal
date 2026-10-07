from collections import deque
g = {0: [1, 4], 1: [0, 2, 5], 2: [1, 3], 3: [2, 6], 4: [0, 5], 5: [1, 4, 6], 6: [3, 5]}
seen = {0}; q = deque([0]); order = []
while q:
    u = q.popleft(); order.append(u)
    for v in sorted(g[u]):
        if v not in seen:
            seen.add(v); q.append(v)
print(" ".join(map(str, order)))
