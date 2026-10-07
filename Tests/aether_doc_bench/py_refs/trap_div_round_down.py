total = 0
for p in [23, 40, 7, 99, 51]:
    r = p // 5 * 5
    total += r
    print(f"{p} -> {r}")
print(f"total rounded = {total}")
