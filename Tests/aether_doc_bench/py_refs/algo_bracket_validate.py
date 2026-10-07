pairs = {")": "(", "]": "[", "}": "{"}
for idx, s in enumerate(["([]{})", "([)]", "((", "", "{[()]}"]):
    st = []; bad = -1
    for p, ch in enumerate(s):
        if ch in "([{": st.append(ch)
        elif ch in pairs:
            if not st or st[-1] != pairs[ch]: bad = p; break
            st.pop()
    if bad >= 0: print(f"{idx}: bad at {bad}")
    elif st: print(f"{idx}: unclosed {len(st)}")
    else: print(f"{idx}: ok")
