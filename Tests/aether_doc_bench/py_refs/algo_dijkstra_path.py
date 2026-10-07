a=[0,0,1,1,2,2,3,4]; b=[1,2,2,3,3,4,5,5]; w=[7,9,10,15,11,2,6,9]
n = 6; INF = float("inf")
dist = [INF]*n; prev = [-1]*n; used = [False]*n; dist[0] = 0
adj = [[] for _ in range(n)]
for x, y, c in zip(a, b, w): adj[x].append((y, c)); adj[y].append((x, c))
for _ in range(n):
    u = -1
    for v in range(n):
        if not used[v] and dist[v] < INF and (u == -1 or dist[v] < dist[u]): u = v
    if u == -1: break
    used[u] = True
    for v, c in adj[u]:
        if dist[u] + c < dist[v]: dist[v] = dist[u] + c; prev[v] = u
for v in range(n): print(f"dist[{v}]={dist[v]}")
path = []; v = 5
while v != -1: path.append(v); v = prev[v]
path.reverse()
print("path:" + "".join(f" {v}" for v in path))
print(f"cost={dist[5]}")
