import bisect

a = [3 * i + 1 for i in range(100000)]
found = index_sum = 0
for j in range(10000):
    q = (j * 7919) % 300007
    k = bisect.bisect_left(a, q)
    if k < len(a) and a[k] == q:
        found += 1
        index_sum += k
print(f"found = {found}")
print(f"index sum = {index_sum}")
