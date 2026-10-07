import re
src = open("bench_consts").read()
c = {m.group(1): int(m.group(2)) for m in re.finditer(r"export\s+const\s+(\w+)\s*:\s*Int\s*=\s*(-?\d+)\s*;", src)}
Base, Step = c["Base"], c["Step"]
print(Base)
print(Base + Step)
print(Base + Step + Step)
