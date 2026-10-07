def grade(s):
    return "A" if s >= 90 else "B" if s >= 80 else "C" if s >= 70 else "F"


passing = 0
for s in [95, 83, 71, 40]:
    g = grade(s)
    passing += g != "F"
    print(f"{s}: {g}")
print(f"passing = {passing}")
