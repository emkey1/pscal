import sys

lines = kept = total = 0
for raw in sys.stdin:
    raw = raw.rstrip("\n")
    if not raw:
        continue
    name, qty = raw.split(" ")
    qty = int(qty)
    lines += 1
    total += qty
    if qty > 0:
        kept += 1
        print(f"{name}: {qty}")
print(f"lines = {lines}")
print(f"kept = {kept}")
print(f"total = {total}")
