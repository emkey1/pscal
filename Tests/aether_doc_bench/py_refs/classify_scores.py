def classify(score):
    if score >= 90: return "ready"
    if score >= 70: return "review"
    return "blocked"
for s in (95, 72, 10):
    print(f"{s} => {classify(s)}")
