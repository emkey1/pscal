cap = 3; cache = []; hits = misses = 0; ev = []
for k in [1,2,3,1,4,2,5,1,3,4]:
    if k in cache:
        hits += 1; cache.remove(k); cache.append(k)
    else:
        misses += 1
        if len(cache) == cap: ev.append(cache.pop(0))
        cache.append(k)
print(f"hits={hits} misses={misses}")
print("evicted:" + "".join(f" {v}" for v in ev))
print(f"resident={len(cache)}")
