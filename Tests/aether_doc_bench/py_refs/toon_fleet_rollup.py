import json
d = json.load(open("fleet.json"))
tc = tm = tagged = 0
for e in d["fleet"]:
    spec = e.get("spec", {})
    cores = spec.get("cores", 0)
    mem = spec.get("mem", {}).get("gb", 0) if isinstance(spec.get("mem"), dict) else 0
    tags = len(e.get("tags", []))
    print(f"{e['id']}: cores={cores} mem={mem} tags={tags}")
    tc += cores; tm += mem; tagged += 1 if tags > 0 else 0
print(f"totals cores={tc} mem={tm} tagged={tagged}")
