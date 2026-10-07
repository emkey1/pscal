import json

with open("projects.json", encoding="utf-8") as fh:
    projects = json.load(fh)["projects"]
total = untagged = 0
for p in projects:
    count = len(p.get("tags", []))
    total += count
    untagged += count == 0
    print(f"{p['name']}: {count} tags")
print(f"total tags = {total}")
print(f"untagged = {untagged}")
