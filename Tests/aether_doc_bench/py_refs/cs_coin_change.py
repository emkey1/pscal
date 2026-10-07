amt = 11; coins = [1, 2, 5]; INF = float("inf")
dp = [0] + [INF] * amt
for a in range(1, amt + 1):
    for c in coins:
        if c <= a and dp[a - c] + 1 < dp[a]:
            dp[a] = dp[a - c] + 1
print(f"coins = {dp[amt] if dp[amt] != INF else -1}")
