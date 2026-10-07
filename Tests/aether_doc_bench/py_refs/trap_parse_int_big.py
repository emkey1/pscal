total = 0
for t in ["42", "2147483647", "3000000000", "9007199254740993"]:
    n = int(t)
    total += n
    print(f"{t} -> next {n + 1}")
print(f"sum = {total}")
