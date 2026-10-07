labels = ["open","fee","refund","adj","close"]; vals = [125000, -3335, 22225, -1, -50005]
def rnd(t):
    a = abs(t); q, r = divmod(a, 10)
    if r >= 5: q += 1
    return -q if t < 0 else q
def fmt(c):
    a = abs(c); s = f"{a // 100}.{a % 100:02d}"
    return f"({s})" if c < 0 else s
bal = 0; posted = 0
for l, v in zip(labels, vals):
    c = rnd(v); bal += c
    if c != 0: posted += 1
    print(f"{l} {fmt(c)} bal={fmt(bal)}")
print(f"posted={posted} of {len(vals)} final={fmt(bal)}")
