def qs(a):
    if len(a) <= 1:
        return a
    p = a[-1]
    return qs([x for x in a[:-1] if x < p]) + [p] + qs([x for x in a[:-1] if x >= p])
print(", ".join(map(str, qs([6, 3, 9, 1, 7, 4, 8, 2, 5]))))
