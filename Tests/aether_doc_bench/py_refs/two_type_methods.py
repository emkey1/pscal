class Point:
    def __init__(self, x, y): self.x, self.y = x, y
    def shiftX(self, dx): return self.x + dx
class Box:
    def __init__(self, w, h): self.width, self.height = w, h
    def area(self): return self.width * self.height
p = Point(3, 4); b = Box(5, 6)
print(f"shiftedX={p.shiftX(10)}")
print(f"area={b.area()}")
print(f"combined={p.shiftX(10) + b.area()}")
