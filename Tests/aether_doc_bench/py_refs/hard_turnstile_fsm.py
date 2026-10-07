import json
ev = json.load(open("events_hard_g.json"))["events"]
st = "locked"; passes = denied = 0
for i, x in enumerate(ev):
    e = x["e"]
    if st == "locked":
        if e == "coin":
            st = "unlocked"; print(f"event {i}: coin -> unlocked")
        else:
            denied += 1; print(f"event {i}: push -> DENIED")
    else:
        if e == "push":
            st = "locked"; passes += 1; print(f"event {i}: push -> PASS")
        else:
            print(f"event {i}: coin -> thanks")
print(f"passes = {passes}")
print(f"denied = {denied}")
print(f"final state = {st}")
