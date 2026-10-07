for n in [4729, 305, 81]:
    a, b, c, d = (n // 1000) % 10, (n // 100) % 10, (n // 10) % 10, n % 10
    print(f"{n}: thousands={a} hundreds={b} tens={c} ones={d} sum={a + b + c + d}")
