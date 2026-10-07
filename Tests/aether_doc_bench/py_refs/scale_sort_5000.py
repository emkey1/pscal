x = 42
xs = []
for _ in range(5000):
    x = (1103515245 * x + 12345) % 2147483648
    xs.append(x % 100000)
xs.sort()
print("first = " + " ".join(map(str, xs[:5])))
print("last = " + " ".join(map(str, xs[-5:])))
print(f"median = {(xs[2499] + xs[2500]) // 2}")
print(f"checksum = {sum(k * v for k, v in enumerate(xs)) % 1000000007}")
