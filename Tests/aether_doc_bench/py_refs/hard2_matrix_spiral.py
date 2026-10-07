N = 4
g = [0] * (N * N)
top, bot, left, right = 0, N - 1, 0, N - 1
v = 0
while top <= bot and left <= right:
    for c in range(left, right + 1): v += 1; g[top * N + c] = v
    top += 1
    for r in range(top, bot + 1): v += 1; g[r * N + right] = v
    right -= 1
    if top <= bot:
        for c in range(right, left - 1, -1): v += 1; g[bot * N + c] = v
        bot -= 1
    if left <= right:
        for r in range(bot, top - 1, -1): v += 1; g[r * N + left] = v
        left += 1
for r in range(N):
    print(f"row {r}: " + " ".join(str(g[r * N + c]) for c in range(N)))
print(f"total = {v}")
