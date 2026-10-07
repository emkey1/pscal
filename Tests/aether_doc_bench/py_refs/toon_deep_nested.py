import json
doc = json.load(open("rosters.json"))
total = 0
for i, t in enumerate(doc["teams"]):
    n = len(t["members"]); total += n
    print(f"team {i}: {t['name']} / {n}")
print(f"total members = {total}")
