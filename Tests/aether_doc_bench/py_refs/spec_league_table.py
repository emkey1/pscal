import json
teams = json.load(open("league.json"))["teams"]
rows = []
for t in teams:
    st = t.get("stats") or {}
    gf = st.get("gf", 0); ga = st.get("ga", 0)
    rows.append((t["name"], t["won"]*3 + t["drawn"], gf - ga, gf))
rows.sort(key=lambda r: (-r[1], -r[2], -r[3], r[0]))
ranks = []
for i, r in enumerate(rows):
    if i > 0 and r[1:] == rows[i-1][1:]: ranks.append(ranks[-1])
    else: ranks.append(i + 1)
for r, (n, p, d, f) in zip(ranks, rows): print(f"{r}. {n} pts={p} gd={d} gf={f}")
tied = sum(1 for i in range(1, len(rows)) if ranks[i] == ranks[i-1])
print(f"teams={len(rows)} tiedPairs={tied} topRank={ranks[0]}")
