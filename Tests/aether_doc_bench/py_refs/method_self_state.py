class Counter:
    def __init__(self, label):
        self.hits = 0; self.label = label
    def add(self, n): self.hits += n
    def describe(self, prefix): return f"{prefix}{self.label}={self.hits}"
c = Counter("clicks")
for a in [3, 1, 4, 1, 5]:
    c.add(a)
print(c.describe("counter "))
print(f"final={c.hits}")
