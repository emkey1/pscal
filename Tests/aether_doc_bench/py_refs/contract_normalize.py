def normalize(score):
    assert score >= 0
    r = max(0, min(100, score))
    assert 0 <= r <= 100
    return r
for s in (150, 42):
    print(f"{s} => {normalize(s)}")
