a = []
for i in range(1, 2001):
    a.append(i * i)
s = 0
for x in a:
    s += x
print(f"count = {len(a)}")
print(f"first = {a[0]}")
print(f"last = {a[-1]}")
print(f"sum = {s}")
