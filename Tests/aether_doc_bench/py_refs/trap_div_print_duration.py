def describe(s):
    return f"{s // 3600}h {(s % 3600) // 60}m {s % 60}s"


total = 0
for s in [3725, 59, 86399]:
    total += s
    print(f"{s}s = {describe(s)}")
print(f"total = {describe(total)}")
