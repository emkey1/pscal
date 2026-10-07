class Gauge:
    def __init__(self, name="boiler", level=40): self.name, self.level = name, level
for g in (Gauge(), Gauge(level=75)):
    print(f"{g.name} {g.level}")
