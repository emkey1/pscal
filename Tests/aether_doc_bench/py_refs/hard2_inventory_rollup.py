from decimal import Decimal, ROUND_HALF_UP
class Item:
    def __init__(self, name, category, qty, unitPrice):
        self.name, self.category, self.qty, self.unitPrice = name, category, qty, unitPrice
    def lineValue(self): return self.qty * self.unitPrice
items = [Item("bolt","fastener",100,0.25), Item("nut","fastener",250,0.1),
         Item("hammer","tool",4,12.5), Item("wrench","tool",7,9.75),
         Item("screw","fastener",500,0.05), Item("saw","tool",2,22.0)]
def f2(x): return str(Decimal(repr(round(x, 9))).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))
order = []; cnt = {}; tot = {}
for it in items:
    if it.category not in cnt:
        order.append(it.category); cnt[it.category] = 0; tot[it.category] = 0.0
    cnt[it.category] += 1; tot[it.category] += it.lineValue()
grand = 0.0
for c in order:
    print(f"category {c}: items={cnt[c]} value={f2(tot[c])}")
    grand += tot[c]
print(f"grand total = {f2(grand)}")
print(f"categories = {len(order)}")
