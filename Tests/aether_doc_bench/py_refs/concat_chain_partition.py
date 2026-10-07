xs = [5, 2, 9, 4, 7, 6]
def evens(a): return [x for x in a if x % 2 == 0]
def odds(a): return [x for x in a if x % 2 != 0]
m = evens(xs) + [0] + odds(xs) + [99]
print("merged:" + "".join(f" {v}" for v in m))
print(f"len={len(m)}")
