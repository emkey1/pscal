DIGITS = "0123456789ABCDEF"


def to_base(n, base):
    if n == 0:
        return "0"
    out = ""
    while n > 0:
        out = DIGITS[n % base] + out
        n //= base
    return out


for n in [10, 255, 1000]:
    for b in [2, 8, 16]:
        print(f"{n} in base {b} = {to_base(n, b)}")
