xs = [12, 5, 30, 8, 21]
def minmax(a): return (min(a), max(a))
def sumcount(a): return (sum(a), len(a))
lo, hi = minmax(xs)
sc = sumcount(xs)
mean = sc[0] / sc[1]
print(f"lo={lo} hi={hi} span={hi-lo}")
print(f"sum={sc[0]} n={sc[1]} mean={mean:.2f}")
print(f"floorMean={int(mean)}")
