def ordered(a, b):
    r = (a, b) if a >= b else (b, a)
    assert r[0] >= r[1]
    return r
hi, lo = ordered(3, 8)
print(f"hi={hi}")
print(f"lo={lo}")
