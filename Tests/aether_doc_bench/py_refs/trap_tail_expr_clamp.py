unchanged = 0
for x in [-5, 42, 100, 250]:
    r = max(0, min(100, x))
    unchanged += r == x
    print(f"clampPct({x}) = {r}")
print(f"unchanged = {unchanged}")
