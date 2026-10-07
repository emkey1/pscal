def digitStats(n):
    if n < 10: return (1, n)
    c, s = digitStats(n // 10)
    return (c + 1, s + n % 10)
d, s = digitStats(48627)
print(f"digits = {d}")
print(f"sum = {s}")
