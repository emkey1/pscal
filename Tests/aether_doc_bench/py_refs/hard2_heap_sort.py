heap = []
def push(v):
    heap.append(v); i = len(heap) - 1
    while i > 0:
        p = (i - 1) // 2
        if heap[p] <= heap[i]: break
        heap[p], heap[i] = heap[i], heap[p]; i = p
def pop():
    root = heap[0]
    last = heap.pop()
    if heap:
        heap[0] = last; i = 0; n = len(heap)
        while True:
            l, r, m = 2 * i + 1, 2 * i + 2, i
            if l < n and heap[l] < heap[m]: m = l
            if r < n and heap[r] < heap[m]: m = r
            if m == i: break
            heap[m], heap[i] = heap[i], heap[m]; i = m
    return root
for v in [42, 7, 19, 3, 88, 15, 4, 61, 23, 8]: push(v)
out = []
while heap: out.append(pop())
ok = all(out[k] <= out[k + 1] for k in range(len(out) - 1))
print("sorted = " + " ".join(map(str, out)))
print(f"min = {out[0]}")
print(f"max = {out[-1]}")
print(f"ordered = {'true' if ok else 'false'}")
