import re
fields = ["42", "7x", "", "-13", "100", "3.5"]
acc = rej = total = 0
for f in fields:
    if re.fullmatch(r"-?[0-9]+", f):
        acc += 1; total += int(f)
    else:
        rej += 1; print(f"reject: {f}")
print(f"accepted={acc} rejected={rej}")
print(f"sum={total}")
