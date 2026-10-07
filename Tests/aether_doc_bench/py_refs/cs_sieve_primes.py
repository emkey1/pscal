N = 30
p = [True] * N; p[0] = p[1] = False
for i in range(2, N):
    if p[i]:
        for j in range(i * i, N, i):
            p[j] = False
print(", ".join(str(i) for i in range(N) if p[i]))
