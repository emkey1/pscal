s, t = "kitten", "sitting"
m, n = len(s), len(t)
d = [[0]*(n+1) for _ in range(m+1)]
for i in range(m+1): d[i][0] = i
for j in range(n+1): d[0][j] = j
for i in range(1, m+1):
    for j in range(1, n+1):
        c = 0 if s[i-1] == t[j-1] else 1
        d[i][j] = min(d[i-1][j-1]+c, d[i][j-1]+1, d[i-1][j]+1)
ops = []; i, j = m, n
while i > 0 or j > 0:
    if i > 0 and j > 0 and s[i-1] == t[j-1] and d[i][j] == d[i-1][j-1]:
        i -= 1; j -= 1
    elif i > 0 and j > 0 and d[i][j] == d[i-1][j-1] + 1:
        ops.append(f"sub {s[i-1]}->{t[j-1]}"); i -= 1; j -= 1
    elif j > 0 and d[i][j] == d[i][j-1] + 1:
        ops.append(f"ins {t[j-1]}"); j -= 1
    else:
        ops.append(f"del {s[i-1]}"); i -= 1
ops.reverse()
print(f"distance={d[m][n]}")
for k, op in enumerate(ops): print(f"{k}: {op}")
