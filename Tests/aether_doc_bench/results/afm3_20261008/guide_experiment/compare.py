import json, glob, collections, re, sys, os
sys.path.insert(0, "<scratch>/lanes/umb-l10/tools")
import aether_doc_bench as adb
E = os.path.dirname(os.path.abspath(__file__))
PATTERNS = {
    "return": r"'return' is not Aether syntax",
    "const-data": r"const declaration requires",
    "for/while": r"'(for|while)' is not Aether|unexpected token '(for|while)'",
    "fx": r"\[FX-001\]",
    "scope": r"\[SCOPE-001\]",
    "other-syn": r"\[SYN-001\]",
}
rows = collections.defaultdict(lambda: collections.Counter())
order = []
for f in sorted(glob.glob(f"{E}/*__*.json")):
    v, suite = os.path.basename(f)[:-5].split("__")
    if v not in order: order.append(v)
    r = json.load(open(f))
    for d in r["destinations"]:
        for var in d["variants"]:
            for c in var["results"]:
                R = rows[v]
                if c["attempts"] and c["attempts"][0].get("not_sent"):
                    R["notsent"] += 1; continue
                R["repover"] += any(x.get("not_sent") for x in c["attempts"][1:])
                a = c["attempts"][0]; cls = adb.classify_attempt(a)
                R["n"] += 1; R["n_" + suite] += 1
                R["fa"] += cls == "pass"; R["fa_" + suite] += cls == "pass"
                R["fx"] += bool((c.get("run") or {}).get("exact_stdout_match"))
                R["silent"] += cls == "silent_wrong"
                st = ((a.get("run") or {}).get("stderr") or "").splitlines()
                first = st[0] if st else ""
                for k, p in PATTERNS.items():
                    if re.search(p, first): R[k] += 1; break
                R["ptok"] = max(R["ptok"], (a.get("usage") or {}).get("prompt_tokens") or 0)
hdr = "%-20s %5s %8s %8s %6s | %s" % ("variant", "sent", "FA", "FX", "silent", "  ".join("%-9s" % k for k in PATTERNS))
print(hdr); print("-" * len(hdr))
for v in order:
    R = rows[v]
    per = " ".join("%s %d/%d" % (s.split("_")[1][:4], R["fa_" + s], R["n_" + s]) for s in ("tasks_v2_pos", "tasks_hard_v2", "tasks_cs") if R["n_" + s])
    print("%-20s %5d %8s %8s %6d | %s   [%s] max prompt %d" % (v, R["n"], "%d" % R["fa"], "%d" % R["fx"], R["silent"],
          "  ".join("%-9d" % R[k] for k in PATTERNS), per, R["ptok"]) + ("  NOT SENT %d" % R["notsent"] if R["notsent"] else "") + ("  repair-overflow %d" % R["repover"] if R["repover"] else ""))
