s = "red-green-blue"
parts = []; start = 0
for i, ch in enumerate(s):
    if ch == "-":
        parts.append(s[start:i]); start = i + 1
parts.append(s[start:])
for i, p in enumerate(parts):
    cap = p[:1].upper() + p[1:]
    print(f"{i}:{cap} len={len(p)}")
print(f"count={len(parts)}")
