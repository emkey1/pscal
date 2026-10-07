scores = [75, 50, 90, 40]
p = sum(1 for s in scores if s >= 60)
print(f"passed = {p}")
print(f"failed = {len(scores) - p}")
