import json
d = json.load(open("expenses_hard_a.json"))
ex = d["expenses"]
mean = sum(e["amount"] for e in ex) / len(ex)
outl = 0
maxdev = 0.0
for i, e in enumerate(ex):
    dev = abs(e["amount"] - mean)
    o = dev > mean / 2
    if o:
        outl += 1
    maxdev = max(maxdev, dev)
    print(f"expense {i}: {e['name']} / {e['amount']} / dev {dev:.2f} / {'true' if o else 'false'}")
print(f"count = {len(ex)}")
print(f"mean = {mean:.2f}")
print(f"outliers = {outl}")
print(f"maxDev = {maxdev:.2f}")
