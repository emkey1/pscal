"""Every accepted unit must hold one case per task of its suite, per guide variant.
usage: audit_units.py RESULTS_DIR...   (prints only units that are short)"""
import glob, gzip, json, os, sys
EXPECT = {"tasks_v2_pos": 35, "tasks_hard_v2": 14, "tasks_hard_nontoon": 5, "tasks_cs": 19, "tasks_traps": 24}
bad = total = 0
for d in sys.argv[1:]:
    for p in sorted(glob.glob(os.path.join(d, "*", "*__r[0-9].json")) + glob.glob(os.path.join(d, "*", "*__r[0-9].json.gz"))):
        r = json.loads(gzip.decompress(open(p, "rb").read()) if p.endswith(".gz") else open(p).read())
        suite = os.path.basename(p).split("__")[1]; total += 1
        for v in r["destinations"][0]["variants"]:
            res = v.get("results") or v.get("python_baseline_results") or []
            want = EXPECT.get(suite)
            if want and len(res) != want:
                bad += 1; print(f"SHORT {p}: {v.get('doc_name')} has {len(res)} of {want} cases; resumed {len(r.get('resumed') or [])}")
print(f"{total} unit reports checked, {bad} short")
