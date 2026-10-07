import json
deps = json.load(open("org_hard_e.json"))["departments"]
tm = tp = 0
top = None
for i, d in enumerate(deps):
    n = len(d["members"]); s = sum(m["salary"] for m in d["members"])
    print(f"dept {i}: {d['name']} / {n} members / payroll {s}")
    tm += n; tp += s
    if top is None or s > top[1]:
        top = (d["name"], s)
print(f"total members = {tm}")
print(f"total payroll = {tp}")
print(f"top department = {top[0]} ({top[1]})")
