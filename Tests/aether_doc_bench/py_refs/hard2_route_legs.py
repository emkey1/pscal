import json
d = json.loads('{"routes":[{"id":"r1","legs":[{"km":12,"mode":"rail"},{"km":5,"mode":"bus"}]},{"id":"r2","legs":[{"km":30,"mode":"rail"},{"km":8,"mode":"walk"},{"km":4,"mode":"bus"}]},{"id":"r3","legs":[{"km":9,"mode":"walk"}]}]}')
best = None; rail = 0
for r in d["routes"]:
    km = sum(l["km"] for l in r["legs"])
    rail += sum(l["km"] for l in r["legs"] if l["mode"] == "rail")
    print(f"route {r['id']}: legs={len(r['legs'])} km={km} {'>'.join(l['mode'] for l in r['legs'])}")
    if best is None or km > best[1]:
        best = (r["id"], km)
print(f"longest = {best[0]} ({best[1]} km)")
print(f"rail km = {rail}")
