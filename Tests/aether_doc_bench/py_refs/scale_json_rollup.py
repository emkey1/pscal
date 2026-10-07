import json

with open("orders.json", encoding="utf-8") as fh:
    orders = json.load(fh)["orders"]
regions = ["central", "east", "north", "south", "west"]
agg = {r: [0, 0, 0] for r in regions}
best = None
for o in orders:
    value = o["qty"] * o["cents"]
    a = agg[o["region"]]
    a[0] += 1
    a[1] += o["qty"]
    a[2] += value
    if best is None or value > best[0] or (value == best[0] and o["id"] < best[1]):
        best = (value, o["id"])
for r in regions:
    c, q, v = agg[r]
    print(f"{r}: orders={c} qty={q} revenue={v // 100}.{v % 100:02d}")
print(f"orders = {len(orders)}")
print(f"largest = {best[1]}")
