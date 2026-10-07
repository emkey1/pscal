from decimal import Decimal, ROUND_HALF_UP
s = [70, 80, 95]
t = sum(s)
mean = (Decimal(t) / Decimal(len(s))).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP)
print(f"total = {t}")
print(f"average = {mean}")
