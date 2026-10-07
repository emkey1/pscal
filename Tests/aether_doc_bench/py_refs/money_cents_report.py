from decimal import Decimal, ROUND_HALF_UP
items = [("widget", 1999), ("gadget", 550), ("bolt", 7)]
def amt(c): return f"{c // 100}.{c % 100:02d}"
total = 0
for l, c in items:
    print(f"{l}: {amt(c)}"); total += c
print(f"total={amt(total)}")
avg = Decimal(total) / Decimal(len(items))
dollars = (avg / 100).quantize(Decimal("0.001"), rounding=ROUND_HALF_UP)
rounded = int(avg.quantize(Decimal("1"), rounding=ROUND_HALF_UP))
print(f"avg={dollars} rounded={rounded}")
