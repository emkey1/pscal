def encode(s):
    out = []; i = 0
    while i < len(s):
        j = i
        while j < len(s) and s[j] == s[i]: j += 1
        out.append(f"{s[i]}{j - i}"); i = j
    return "".join(out)
def decode(e):
    out = []; i = 0
    while i < len(e):
        ch = e[i]; i += 1; j = i
        while j < len(e) and e[j].isdigit(): j += 1
        out.append(ch * int(e[i:j])); i = j
    return "".join(out)
inputs = ["aaabbbcccd", "wwwwwwwwwwww", "abc", "z" * 22]
for i, s in enumerate(inputs):
    enc = encode(s)
    print(f"case {i}: {s}")
    print(f"  encoded = {enc}")
    print(f"  roundtrip_ok = {'true' if decode(enc) == s else 'false'}")
