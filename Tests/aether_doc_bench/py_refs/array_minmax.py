a = [12, 5, 23, 8, 19]
mn = mx = a[0]
for i in range(len(a)):
    mn = min(mn, a[i]); mx = max(mx, a[i])
print(f"min = {mn}")
print(f"max = {mx}")
print(f"range = {mx - mn}")
