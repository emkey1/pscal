n = 27; s = 0
while n != 1:
    n = n // 2 if n % 2 == 0 else 3 * n + 1
    s += 1
print(f"steps = {s}")
