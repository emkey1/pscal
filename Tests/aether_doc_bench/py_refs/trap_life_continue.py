rows = ["......", "..#...", "...#..", ".###..", "......", "......"]
g = [[1 if ch == "#" else 0 for ch in line] for line in rows]
R, C = len(g), len(g[0])


def neighbours(r, c):
    return sum(g[r + dr][c + dc]
               for dr in (-1, 0, 1) for dc in (-1, 0, 1)
               if (dr or dc) and 0 <= r + dr < R and 0 <= c + dc < C)


counts = [[neighbours(r, c) for c in range(C)] for r in range(R)]
nxt = [[1 if (g[r][c] and counts[r][c] in (2, 3)) or (not g[r][c] and counts[r][c] == 3) else 0
        for c in range(C)] for r in range(R)]
for row in counts:
    print("".join(str(n) for n in row))
for row in nxt:
    print("".join("#" if v else "." for v in row))
print(f"alive = {sum(map(sum, nxt))}")
