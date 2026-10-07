from functools import cmp_to_key
vs = ["1.2.10","1.2.9","1.3.0-alpha","1.3.0","1.3.0-beta","0.9.9"]
def parse(v):
    core, sep, pre = v.partition("-")
    return [int(x) for x in core.split(".")], (pre if sep else None)
def cmp(a, b):
    (ca, pa), (cb, pb) = parse(a), parse(b)
    if ca != cb: return -1 if ca < cb else 1
    if pa == pb: return 0
    if pa is None: return 1
    if pb is None: return -1
    return -1 if pa < pb else 1
s = sorted(vs, key=cmp_to_key(cmp))
for i, v in enumerate(s): print(f"{i}: {v}")
print(f"lowest={s[0]} highest={s[-1]}")
