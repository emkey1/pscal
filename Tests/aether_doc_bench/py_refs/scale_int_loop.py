x, total = 1, 0
for _ in range(10000000):
    x = (1103515245 * x + 12345) % 2147483648
    total += x % 7
print(f"x = {x}")
print(f"total = {total}")
