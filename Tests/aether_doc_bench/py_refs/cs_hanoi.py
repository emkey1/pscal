def h(n, a, c, b):
    if n == 0:
        return
    h(n - 1, a, b, c)
    print(f"move disk {n} from {a} to {c}")
    h(n - 1, b, c, a)
h(3, "A", "C", "B")
