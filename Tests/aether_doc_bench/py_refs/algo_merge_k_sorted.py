arrs = [[1,4,9],[2,3,10,11],[5,6]]
pos = [0]*len(arrs); out = []
while True:
    best = -1
    for k, a in enumerate(arrs):
        if pos[k] < len(a) and (best == -1 or a[pos[k]] < arrs[best][pos[best]]): best = k
    if best == -1: break
    out.append(arrs[best][pos[best]]); pos[best] += 1
ok = all(out[i] <= out[i+1] for i in range(len(out)-1))
print("merged:" + "".join(f" {v}" for v in out))
print(f"len={len(out)} sorted={'true' if ok else 'false'}")
