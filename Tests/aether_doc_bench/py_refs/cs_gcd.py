a, b = 48, 36
while b:
    a, b = b, a % b
print(f"gcd(48, 36) = {a}")
