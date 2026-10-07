line = 'a,"b,c","say ""hi""", d ,,"end"'
fields = []; cur = ""; inq = False; i = 0
while i < len(line):
    ch = line[i]
    if inq:
        if ch == '"':
            if i + 1 < len(line) and line[i+1] == '"': cur += '"'; i += 1
            else: inq = False
        else: cur += ch
    else:
        if ch == '"': inq = True
        elif ch == ",": fields.append(cur); cur = ""
        else: cur += ch
    i += 1
fields.append(cur)
for i, f in enumerate(fields): print(f"{i}: '{f}' len={len(f)}")
print(f"fields={len(fields)}")
