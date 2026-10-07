def spread(xs):
    assert all(x > 0 for x in xs)
    r = max(xs) - min(xs)
    assert r >= 0
    return r
print(f"spread={spread([4, 19, 7, 2, 11])}")
print(f"single={spread([5])}")
