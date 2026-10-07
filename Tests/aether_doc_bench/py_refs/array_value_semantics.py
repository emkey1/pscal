def scale(xs, k): return [x * k for x in xs]
orig = [1, 2, 3]
a = scale(orig, 10); b = scale(a, 3)
for lab, arr in (("scaled:", a), ("twice:", b), ("original:", orig)):
    print(lab + "".join(f" {v}" for v in arr))
