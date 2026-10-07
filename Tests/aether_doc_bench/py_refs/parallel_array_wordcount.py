words = ["ash", "birch", "ash", "cedar", "birch", "ash"]
keys, counts = [], []
for w in words:
    if w in keys: counts[keys.index(w)] += 1
    else: keys.append(w); counts.append(1)
for k, c in zip(keys, counts): print(f"{k}={c}")
print(f"distinct={len(keys)}")
bi = 0
for i in range(len(keys)):
    if counts[i] > counts[bi]: bi = i
print(f"most={keys[bi]} ({counts[bi]})")
