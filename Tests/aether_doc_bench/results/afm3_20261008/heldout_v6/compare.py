import json, glob, collections, sys, os
sys.path.insert(0, "<scratch>/lanes/umb-l10/tools")
import aether_doc_bench as adb
B = "<scratch>/apple-bench"
def load(pattern):
    out = {}
    for f in glob.glob(pattern):
        for c in json.load(open(f))["destinations"][0]["variants"][0]["results"]:
            a = c["attempts"][0]
            if a.get("not_sent"): cls = "not_sent"
            else: cls = adb.classify_attempt(a)
            fx = bool((c.get("run") or {}).get("exact_stdout_match"))
            out[(c["task_id"], c["repeat_index"])] = (cls, fx)
    return out
v6 = load(B + "/heldout_v6/v6__*.json")
v0 = load(B + "/afm3_card_20261008/afm3__*.json")
keys = sorted(set(v6) & set(v0))
def tally(d):
    c = collections.Counter(d[k][0] for k in keys); fx = sum(d[k][1] for k in keys)
    return c, fx
c0, f0 = tally(v0); c6, f6 = tally(v6)
print(f"matched cases: {len(keys)} (tasks {len({k[0] for k in keys})})")
print(f"{'':12s} {'V0 card':>10s} {'V6 table':>10s}")
for k in ("pass", "silent_wrong", "coded_error", "uncoded_error", "not_sent"):
    print(f"{k:12s} {c0.get(k,0):10d} {c6.get(k,0):10d}")
print(f"{'after repair':12s} {f0:10d} {f6:10d}")
won = sorted({k[0] for k in keys if v6[k][0] == 'pass' and v0[k][0] != 'pass'}); lost = sorted({k[0] for k in keys if v0[k][0] == 'pass' and v6[k][0] != 'pass'})
print("V6 gains on:", won); print("V6 losses on:", lost)
# paired sign test on cases where they differ in pass
b = sum(1 for k in keys if v6[k][0] == 'pass' and v0[k][0] != 'pass'); c = sum(1 for k in keys if v0[k][0] == 'pass' and v6[k][0] != 'pass')
from math import comb
n = b + c
p = sum(comb(n, i) for i in range(max(b, c), n + 1)) / 2 ** n * 2 if n else 1.0
print(f"discordant: V6-only {b}, V0-only {c}, two-sided sign test p={min(p,1):.3f}")
