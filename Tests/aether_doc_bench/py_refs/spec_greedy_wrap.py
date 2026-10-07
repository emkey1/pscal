words = ["the","quick","brown","fox","jumped","over","lazy","dogs","again"]; W = 16
lines = []; cur = ""
for w in words:
    if cur == "": cur = w
    elif len(cur) + 1 + len(w) <= W: cur += " " + w
    else: lines.append(cur); cur = w
if cur: lines.append(cur)
for i, l in enumerate(lines, 1): print(f"{i}|{l}| len={len(l)}")
print(f"lines={len(lines)} width={W} longest={max(len(l) for l in lines)}")
