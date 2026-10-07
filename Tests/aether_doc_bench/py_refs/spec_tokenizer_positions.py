# Tracked ambiguity (W1-11): the prompt never names the string token kind;
# this oracle written from the prompt alone printed `string`. It follows the
# task's expected `str` until the prompt says so (py_refs/ambiguities.json).
src = 'set x = 12; # note\nsay "hi there"; say "bad'
toks = []; unterm = 0
i = 0; line = 1; col = 1; n = len(src)
while i < n:
    ch = src[i]
    if ch == "\n":
        i += 1; line += 1; col = 1
    elif ch == " ":
        i += 1; col += 1
    elif ch == "#":
        while i < n and src[i] != "\n": i += 1; col += 1
    elif ch == ";":
        toks.append((line, col, "semi", ";")); i += 1; col += 1
    elif ch == '"':
        sl, sc = line, col
        j = i + 1
        while j < n and src[j] != '"': j += 1
        if j >= n:
            unterm += 1
            for c in src[i:]:
                if c == "\n": line += 1; col = 1
                else: col += 1
            i = n
        else:
            val = src[i+1:j]
            toks.append((sl, sc, "str", val))
            for c in src[i:j+1]:
                if c == "\n": line += 1; col = 1
                else: col += 1
            i = j + 1
    else:
        sl, sc = line, col; j = i
        while j < n and src[j] not in ' ;\n#"': j += 1
        toks.append((sl, sc, "word", src[i:j])); col += j - i; i = j
for l, c, k, v in toks: print(f"{l}:{c} {k} '{v}'")
print(f"tokens={len(toks)} unterminated={unterm}")
