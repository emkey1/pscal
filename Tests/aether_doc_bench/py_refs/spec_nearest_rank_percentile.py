a = sorted([15,20,35,40,50,20]); n = len(a)
print("sorted:" + "".join(f" {v}" for v in a))
for p in [0,25,50,75,100,30]:
    r = (p * n + 99) // 100
    if r < 1: r = 1
    print(f"p{p} rank={r} value={a[r-1]}")
print(f"n={n}")
