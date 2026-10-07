def phrase(s):
    if s == 0: return "0 seconds"
    d, r = divmod(s, 86400); h, r = divmod(r, 3600); m, sec = divmod(r, 60)
    parts = []
    for v, u in ((d, "day"), (h, "hour"), (m, "minute"), (sec, "second")):
        if v: parts.append(f"{v} {u}" + ("" if v == 1 else "s"))
    if len(parts) == 1: return parts[0]
    return ", ".join(parts[:-1]) + " and " + parts[-1]
for s in [0, 1, 61, 3661, 90061, 7200, 86400]: print(f"{s} -> {phrase(s)}")
