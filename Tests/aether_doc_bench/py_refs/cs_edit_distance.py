s, t = "kitten", "sitting"
dp = [[0] * (len(t) + 1) for _ in range(len(s) + 1)]
for i in range(len(s) + 1): dp[i][0] = i
for j in range(len(t) + 1): dp[0][j] = j
for i in range(1, len(s) + 1):
    for j in range(1, len(t) + 1):
        dp[i][j] = min(dp[i-1][j] + 1, dp[i][j-1] + 1, dp[i-1][j-1] + (s[i-1] != t[j-1]))
print(f"distance = {dp[len(s)][len(t)]}")
