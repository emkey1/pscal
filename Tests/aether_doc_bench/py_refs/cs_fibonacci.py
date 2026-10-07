f = [0, 1]
while len(f) < 10:
    f.append(f[-1] + f[-2])
print(", ".join(map(str, f)))
