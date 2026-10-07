tbl = [(1000,"M"),(900,"CM"),(500,"D"),(400,"CD"),(100,"C"),(90,"XC"),(50,"L"),(40,"XL"),(10,"X"),(9,"IX"),(5,"V"),(4,"IV"),(1,"I")]
val = {"I":1,"V":5,"X":10,"L":50,"C":100,"D":500,"M":1000}
def to_r(n):
    s = ""
    for v, r in tbl:
        while n >= v: s += r; n -= v
    return s
def parse(s):
    t = 0
    for i, c in enumerate(s):
        if i + 1 < len(s) and val[c] < val[s[i+1]]: t -= val[c]
        else: t += val[c]
    return t
fail = 0
for n in [4, 9, 14, 40, 1994, 3888, 2024]:
    r = to_r(n); p = parse(r)
    if p != n: fail += 1
    print(f"{n} -> {r} -> {p}")
print(f"roundtripFailures={fail}")
