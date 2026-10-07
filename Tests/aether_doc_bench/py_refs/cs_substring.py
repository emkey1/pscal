text, pat = "mississippi", "sip"
idx = -1
for i in range(len(text) - len(pat) + 1):
    j = 0
    while j < len(pat) and text[i + j] == pat[j]:
        j += 1
    if j == len(pat):
        idx = i; break
print(f"index = {idx}")
