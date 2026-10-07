class Inventory:
    def __init__(self, cur, cap): self.currentStock, self.maxCapacity = cur, cap
    def addItem(self, amount):
        self.currentStock = min(self.currentStock + amount, self.maxCapacity)
        return self.currentStock
    def getStatus(self):
        if self.currentStock >= self.maxCapacity: return "FULL"
        if self.currentStock > 0: return "IN_STOCK"
        return "EMPTY"
inv = Inventory(10, 50)
inv.addItem(25)
print(f"Final Stock: {inv.currentStock}")
print(f"Current Status: {inv.getStatus()}")
