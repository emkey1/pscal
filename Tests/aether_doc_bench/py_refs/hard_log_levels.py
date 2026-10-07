import json
ev = json.load(open("events_hard_b.json"))["events"]
c = {"info": 0, "warn": 0, "error": 0}
first = None
for i, e in enumerate(ev):
    print(f"event {i}: {e['level']} / {e['msg']}")
    c[e["level"]] += 1
    if e["level"] == "error" and first is None:
        first = i
for k in ("info", "warn", "error"):
    print(f"{k} = {c[k]}")
dom = "info"
for k in ("warn", "error"):
    if c[k] > c[dom]:
        dom = k
print(f"dominant = {dom}")
print(f"first error at index {first}" if first is not None else "first error: none")
