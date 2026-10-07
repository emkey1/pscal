vals = [0,5,10,15,20,25,30,9,10,29,30]
cnt = [0, 0, 0]; outside = 0
for v in vals:
    if v < 0 or v > 30: outside += 1
    elif v >= 20: cnt[2] += 1
    else: cnt[v // 10] += 1
for b in range(3):
    lo = b * 10; closer = "]" if b == 2 else ")"
    print(f"[{lo},{lo+10}{closer} {cnt[b]} {'#' * cnt[b]}")
print(f"outside={outside} total={len(vals)}")
