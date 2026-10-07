def validName(name): return 1 <= len(name) <= 6 and name != "bad"
b = lambda x: "true" if x else "false"
print(f"alpha={b(validName('alpha'))}")
print(f"empty={b(validName(''))}")
print(f"bad={b(validName('bad'))}")
