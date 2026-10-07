from collections import Counter

with open("words.txt", encoding="utf-8") as fh:
    words = fh.read().split()
counts = Counter(words)
print(f"words = {len(words)}")
print(f"distinct = {len(counts)}")
top = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:10]
for rank, (w, c) in enumerate(top, 1):
    print(f"{rank}. {w} {c}")
