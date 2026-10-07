xs = [4, 8, 15, 16, 23, 42, 7]
mid = len(xs) // 2
print(" ".join(f"[{x}]" if i == mid else str(x) for i, x in enumerate(xs)))
print(f"middle index = {mid}")
