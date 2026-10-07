A = [1,2,2,2,5,7,7,9]
def lb(t):
    lo, hi = 0, len(A)
    while lo < hi:
        m = (lo + hi) // 2
        if A[m] < t: lo = m + 1
        else: hi = m
    return lo
def ub(t):
    lo, hi = 0, len(A)
    while lo < hi:
        m = (lo + hi) // 2
        if A[m] <= t: lo = m + 1
        else: hi = m
    return lo
for t in [2,7,3,0,9]:
    l, u = lb(t), ub(t)
    print(f"{t}: lb={l} ub={u} count={u-l}")
