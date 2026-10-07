N = 6
def solve(r, cols, d1, d2):
    if r == N:
        return 1
    t = 0
    for c in range(N):
        if c in cols or r - c in d1 or r + c in d2:
            continue
        t += solve(r + 1, cols | {c}, d1 | {r - c}, d2 | {r + c})
    return t
print(f"solutions = {solve(0, frozenset(), frozenset(), frozenset())}")
