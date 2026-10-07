a = [10, 9, 2, 5, 3, 7, 101, 18]
dp = [1] * len(a)
for i in range(len(a)):
    for j in range(i):
        if a[j] < a[i]:
            dp[i] = max(dp[i], dp[j] + 1)
print(f"lis = {max(dp)}")
