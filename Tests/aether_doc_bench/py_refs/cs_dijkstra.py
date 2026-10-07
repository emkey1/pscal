import heapq
edges = [(0,1,4), (0,2,1), (1,2,2), (1,3,1), (2,3,5), (3,4,3)]
adj = {}
for u, v, w in edges:
    adj.setdefault(u, []).append((v, w)); adj.setdefault(v, []).append((u, w))
dist = {0: 0}; pq = [(0, 0)]
while pq:
    d, u = heapq.heappop(pq)
    if d > dist.get(u, float("inf")):
        continue
    for v, w in adj.get(u, []):
        if d + w < dist.get(v, float("inf")):
            dist[v] = d + w; heapq.heappush(pq, (d + w, v))
print(f"distance = {dist[4]}")
