a = [1, 3, 5, 7, 9, 11]; x = 7
lo, hi = 0, len(a) - 1; idx = -1
while lo <= hi:
    m = (lo + hi) // 2
    if a[m] == x:
        idx = m; break
    if a[m] < x:
        lo = m + 1
    else:
        hi = m - 1
print(f"index = {idx}")
