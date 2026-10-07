a = [3, 1, 4, 1, 5]
s = 0; m = a[0]
for i in range(len(a)):
    s += a[i]
    if a[i] > m: m = a[i]
print(f"sum={s}")
print(f"max={m}")
print(f"count={len(a)}")
