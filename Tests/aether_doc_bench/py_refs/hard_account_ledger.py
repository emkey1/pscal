import json
d = json.load(open("ops_hard_c.json"))
cap = d["cap"]
bal = 0
applied = rejected = 0
for i, op in enumerate(d["operations"]):
    k, a = op["kind"], op["amount"]
    before = bal
    if k == "deposit":
        bal = min(bal + a, cap)
        print(f"op {i}: deposit {a} -> {bal}")
    elif k == "withdraw":
        if bal >= a:
            bal -= a
            print(f"op {i}: withdraw {a} -> {bal}")
        else:
            print(f"op {i}: withdraw {a} REJECTED (balance {bal})")
    elif k == "interest":
        bal += bal * a // 100
        print(f"op {i}: interest {a}% -> {bal}")
    if bal != before:
        applied += 1
    else:
        rejected += 1
print(f"final balance = {bal}")
print(f"applied = {applied}")
print(f"rejected = {rejected}")
