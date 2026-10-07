lo = [5,1,12,2,20,13]; hi = [7,4,15,6,22,18]
iv = sorted(zip(lo, hi))
merged = []
for s, e in iv:
    if merged and s <= merged[-1][1]:
        merged[-1][1] = max(merged[-1][1], e)
    else:
        merged.append([s, e])
for s, e in merged: print(f"merged [{s},{e}]")
for k in range(1, len(merged)):
    e = merged[k-1][1]; s = merged[k][0]
    print(f"gap [{e},{s}] size={s-e}")
print(f"count={len(merged)} covered={sum(e-s for s, e in merged)}")
