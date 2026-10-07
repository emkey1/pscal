def ms(a):
    if len(a) <= 1:
        return a
    m = len(a) // 2
    l, r = ms(a[:m]), ms(a[m:])
    out = []; i = j = 0
    while i < len(l) and j < len(r):
        if l[i] <= r[j]:
            out.append(l[i]); i += 1
        else:
            out.append(r[j]); j += 1
    return out + l[i:] + r[j:]
print(", ".join(map(str, ms([38, 27, 43, 3, 9, 82, 10]))))
