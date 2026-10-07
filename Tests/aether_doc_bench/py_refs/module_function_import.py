# Mirrors bench_math's exported cube(n) = n * n * n
def cube(n): return n * n * n
for n in (2, 3, 4):
    print(f"cube({n})={cube(n)}")
