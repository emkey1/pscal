class Point:
    def __init__(self, x, y): self.x, self.y = x, y
    def sum(self): return self.x + self.y
p = Point(3, 4)
print(f"sum={p.sum()}")
