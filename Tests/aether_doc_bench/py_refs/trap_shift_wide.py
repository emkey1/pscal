def bit(k):
    return 1 << k


total = 0
for k in [0, 10, 31, 32, 40]:
    total |= bit(k)
    print(f"bit {k} = {bit(k)}")
print(f"all = {total}")
