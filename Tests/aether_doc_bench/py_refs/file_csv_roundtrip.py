import os
with open("ledger_tmp.txt", "w") as f:
    f.write("alpha,10\nbeta,25\ngamma,7\n")
total = 0
with open("ledger_tmp.txt") as f:
    for _ in range(3):
        line = f.readline().rstrip("\n")
        name, val = line.split(",")
        print(f"{name}={val}")
        total += int(val)
print(f"total={total}")
os.remove("ledger_tmp.txt")
