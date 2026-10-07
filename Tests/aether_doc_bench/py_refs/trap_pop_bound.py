stack = []
for v in [10, 20, 30, 40, 50, 60]:
    stack.append(v)
print(f"size = {len(stack)}")
for _ in range(len(stack)):
    print(f"pop {stack.pop()}")
print(f"left = {len(stack)}")
