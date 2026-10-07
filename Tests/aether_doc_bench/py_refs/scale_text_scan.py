import re

with open("corpus.txt", encoding="utf-8") as fh:
    text = fh.read()
words = re.findall(r"[a-z]+", text)
print(f"letters = {sum(ch.isalpha() for ch in text)}")
print(f"vowels = {sum(ch in 'aeiou' for ch in text)}")
print(f"words = {len(words)}")
print(f"longest = {max(map(len, words))}")
