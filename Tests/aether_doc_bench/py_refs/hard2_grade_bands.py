import json
d = json.loads('{"students":[{"n":"ada","s":93},{"n":"bob","s":71},{"n":"cy","s":85},{"n":"di","s":64},{"n":"ed","s":90},{"n":"fi","s":78}]}')
c = {"A": 0, "B": 0, "C": 0, "F": 0}
tot = 0
for st in d["students"]:
    s = st["s"]
    b = "A" if s >= 90 else "B" if s >= 80 else "C" if s >= 70 else "F"
    c[b] += 1; tot += s
    print(f"student {st['n']} {s} -> {b}")
for b in "ABCF":
    print(f"band {b} = {c[b]}")
print(f"mean = {tot / len(d['students']):.2f}")
