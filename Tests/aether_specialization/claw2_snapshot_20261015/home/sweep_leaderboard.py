import json,os
models=["a3b-ml075xL","a3b-ml1xL","a3b-ml2xL","c14b-v8","dsv2lite","granite8b","m24b-ml1xL","m24b-ml2xL","mistral24b-ml1x","mistral24b-ml2x","mistral24b-ml3x","nemo12","q3-32b","q34b","q3ca3b-ml1x"]
def score(tag,nm):
    f="$HOME/gguf_sweep/%s_%s.json"%(tag,nm)
    if not os.path.exists(f): return None
    try:
        d=json.load(open(f)); c=d["destinations"][0]["variants"][0].get("results",[])
        if not c: return None
        cor=sum(1 for x in c if (x.get("run") or {}).get("exact_stdout_match"))
        return (cor,len(c))
    except: return None
rows=[]
for m in models:
    v2=score(m,"v2"); lg=score(m,"large"); cs=score(m,"cs")
    if v2 or lg or cs: rows.append((m,v2,lg,cs))
rows.sort(key=lambda r:-(r[1][0] if r[1] else -1))
def fmt(s): return "%d/%d"%s if s else "-"
done=sum(1 for r in rows if r[1] and r[2] and r[3])
print("COMPLETE=%d STARTED=%d /15"%(done,len(rows)))
for m,v2,lg,cs in rows:
    print("%-16s | v2 %-6s | large %-5s | cs %-6s"%(m,fmt(v2),fmt(lg),fmt(cs)))
