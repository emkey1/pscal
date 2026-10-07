def evaluate(s):
    pos = 0
    def peek():
        return s[pos] if pos < len(s) else ""
    def expr():
        nonlocal pos
        v = term()
        while peek() in ("+", "-") and peek():
            op = s[pos]; pos += 1
            r = term()
            v = v + r if op == "+" else v - r
        return v
    def term():
        nonlocal pos
        v = factor()
        while peek() in ("*", "/") and peek():
            op = s[pos]; pos += 1
            r = factor()
            if op == "*": v = v * r
            elif r != 0:
                q = abs(v) // abs(r)
                v = q if (v >= 0) == (r >= 0) else -q
        return v
    def factor():
        nonlocal pos
        if peek() == "(":
            pos += 1
            v = expr()
            pos += 1  # ')'
            return v
        if peek() == "-":
            pos += 1
            return -factor()
        start = pos
        while pos < len(s) and s[pos].isdigit(): pos += 1
        return int(s[start:pos])
    return expr()
exprs = ["2+3*4", "(2+3)*4", "100/5-7", "2*(3+4)-(10-4)"]
for i, e in enumerate(exprs):
    print(f"expr {i}: {e} = {evaluate(e)}")
