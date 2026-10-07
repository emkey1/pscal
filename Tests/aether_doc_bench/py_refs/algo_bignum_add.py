x, y = "987654321987654321", "123456789123456789"
a = [int(c) for c in reversed(x)]; b = [int(c) for c in reversed(y)]
out = []; carry = 0
for i in range(max(len(a), len(b))):
    s = (a[i] if i < len(a) else 0) + (b[i] if i < len(b) else 0) + carry
    out.append(s % 10); carry = s // 10
if carry: out.append(carry)
s = "".join(str(d) for d in reversed(out))
print(f"digits={len(s)}")
print(f"sum={s}")
