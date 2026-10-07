from collections import deque

maze = ["..#..", ".##.#", ".....", "#.#.#", "...#."]
R, C = len(maze), len(maze[0])


def bfs(goal):
    dist = {(0, 0): 0}
    q = deque([(0, 0)])
    while q:
        cur = q.popleft()
        if cur == goal:
            return dist[cur]
        r, c = cur
        for dr, dc in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            nr, nc = r + dr, c + dc
            if 0 <= nr < R and 0 <= nc < C and maze[nr][nc] == "." and (nr, nc) not in dist:
                dist[(nr, nc)] = dist[cur] + 1
                q.append((nr, nc))
    return -1


for g in [(2, 4), (4, 0), (4, 4)]:
    print(f"steps to ({g[0]},{g[1]}) = {bfs(g)}")
