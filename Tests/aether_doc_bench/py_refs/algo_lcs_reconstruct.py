a, b = "AGGTAB", "GXTXAYB"
m, n = len(a), len(b)
dp = [[0]*(n+1) for _ in range(m+1)]
for i in range(1, m+1):
    for j in range(1, n+1):
        dp[i][j] = dp[i-1][j-1]+1 if a[i-1] == b[j-1] else max(dp[i-1][j], dp[i][j-1])
i, j = m, n; out = []
while i > 0 and j > 0:
    if a[i-1] == b[j-1]:
        out.append(a[i-1]); i -= 1; j -= 1
    elif dp[i-1][j] >= dp[i][j-1]: i -= 1
    else: j -= 1
print(f"len={dp[m][n]}")
print("lcs=" + "".join(reversed(out)))
