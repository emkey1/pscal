for idx, s in enumerate(["4539 1488 0343 6467", "8273 1232 7352 0569", "79927398713", "79927398710"]):
    d = [int(c) for c in s if c.isdigit()]
    tot = 0
    for pos, x in enumerate(reversed(d)):
        if pos % 2 == 1:
            x *= 2
            if x > 9: x -= 9
        tot += x
    print(f"{idx}: len={len(d)} valid={'true' if tot % 10 == 0 else 'false'}")
