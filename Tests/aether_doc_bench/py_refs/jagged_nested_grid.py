g = [[(r + 1) * (c + 1) for c in range(r + 2)] for r in range(3)]
total = 0
for r, row in enumerate(g):
    print(f"row {r} len={len(row)}:" + "".join(f" {v}" for v in row))
    total += sum(row)
print(f"total={total}")
