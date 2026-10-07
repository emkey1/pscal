READ, WRITE, EXEC = 1, 2, 4
def perm(f):
    return ("r" if f & READ else "-") + ("w" if f & WRITE else "-") + ("x" if f & EXEC else "-")
u = 0
for f in [0, 1, 3, 5, 7, 6]:
    print(f"{f} -> {perm(f)}"); u |= f
print(f"union={u} shifted={u << 2}")
