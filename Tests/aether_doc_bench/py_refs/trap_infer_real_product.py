from decimal import Decimal, ROUND_HALF_UP


def two(x):
    return str(Decimal(str(x)).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))


items = [("pen", Decimal("2.5"), 3), ("notebook", Decimal("4.25"), 2), ("stapler", Decimal("11.99"), 1)]
subtotal = Decimal(0)
for name, price, qty in items:
    line = price * qty
    subtotal += line
    print(f"{name}: {qty} x {two(price)} = {two(line)}")
print(f"subtotal = {two(subtotal)}")
