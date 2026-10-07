import json
def fmt(v):
    if isinstance(v, bool): return "true" if v else "false"
    return str(v)
doc = json.load(open("payload.json"))
for k in ("name", "enabled", "count"):
    print(fmt(doc[k]))
