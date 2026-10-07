import re
names = ["ada","bob","cy","dee","eve"]; raw = ["93","71","x8","60","88"]
recs = []; rej = 0
def band(s): return "A" if s >= 90 else "B" if s >= 75 else "C" if s >= 60 else "D"
for n, r in zip(names, raw):
    if re.fullmatch(r"[0-9]+", r): recs.append((n, int(r), band(int(r))))
    else: rej += 1
recs.sort(key=lambda t: -t[1])
for i, (n, s, b) in enumerate(recs): print(f"{i}: {n} {s} {b}")
bands, cnt = [], []
for _, _, b in recs:
    if b in bands: cnt[bands.index(b)] += 1
    else: bands.append(b); cnt.append(1)
for b, c in zip(bands, cnt): print(f"band {b}={c}")
mean = sum(s for _, s, _ in recs) / len(recs)
print(f"kept={len(recs)} rejected={rej} mean={mean:.2f}")
