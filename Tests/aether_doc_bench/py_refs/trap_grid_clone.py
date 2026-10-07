grid = [[r * 4 + c + 1 for c in range(4)] for r in range(4)]
copy = [row[:] for row in grid]
for r in range(4):
    copy[r][r] = 0
for r in range(4):
    print(f"original row {r} sum = {sum(grid[r])}")
for r in range(4):
    print(f"copy row {r} sum = {sum(copy[r])}")
print("original diagonal = " + " ".join(str(grid[r][r]) for r in range(4)))
