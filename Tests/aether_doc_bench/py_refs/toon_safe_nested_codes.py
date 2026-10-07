import json
doc = json.load(open("rows.json"))
missing = 0
for i, row in enumerate(doc["rows"]):
    code = "EMPTY"
    meta = row.get("meta") if isinstance(row, dict) else None
    if isinstance(meta, dict) and isinstance(meta.get("code"), str):
        code = meta["code"]
    if code == "EMPTY": missing += 1
    print(f"row {i} = {code}")
print(f"missing = {missing}")
