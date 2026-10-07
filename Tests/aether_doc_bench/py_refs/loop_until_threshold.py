level = 0
steps = 0
while True:
    level += 7
    steps += 1
    print(f"step {steps}: {level}")
    if level >= 30:
        break
print(f"steps = {steps}")
