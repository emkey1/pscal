W = [3,4,5,2]; V = [30,50,60,20]; cap = 8; n = len(W)
dp = [[0]*(cap+1) for _ in range(n+1)]
for i in range(1, n+1):
    for c in range(cap+1):
        dp[i][c] = dp[i-1][c]
        if W[i-1] <= c: dp[i][c] = max(dp[i][c], dp[i-1][c-W[i-1]] + V[i-1])
c = cap; chosen = []
for i in range(n, 0, -1):
    if dp[i][c] != dp[i-1][c]: chosen.append(i-1); c -= W[i-1]
chosen.sort()
print(f"best={dp[n][cap]}")
print("items:" + "".join(f" {v}" for v in chosen))
print(f"weight={sum(W[i] for i in chosen)} of {cap}")
