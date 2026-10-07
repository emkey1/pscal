import sys

sys.setrecursionlimit(10000)
N = 1000
visited = [False] * N
order = []


def dfs(u):
    visited[u] = True
    order.append(u)
    for v in ((u * 7 + 3) % N, (u * 13 + 11) % N, (u + 1) % N):
        if not visited[v]:
            dfs(v)


dfs(0)
print(f"reachable = {len(order)}")
print("first = " + " ".join(map(str, order[:8])))
print(f"checksum = {sum(p * u for p, u in enumerate(order)) % 1000000007}")
