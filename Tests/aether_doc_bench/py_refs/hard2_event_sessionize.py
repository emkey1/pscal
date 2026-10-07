import json
d = json.loads('{"gap":5,"events":[{"u":"a","t":1},{"u":"a","t":3},{"u":"a","t":20},{"u":"b","t":2},{"u":"b","t":30},{"u":"a","t":22}]}')
gap = d["gap"]; last = {}; sess = {}
for e in d["events"]:
    u, t = e["u"], e["t"]
    if u not in last:
        sess[u] = 1; print(f"event {u}@{t} -> new session 1")
    elif t - last[u] > gap:
        sess[u] += 1; print(f"event {u}@{t} -> new session {sess[u]}")
    else:
        print(f"event {u}@{t} -> continues session {sess[u]}")
    last[u] = t
for u in sess:
    print(f"user {u} sessions = {sess[u]}")
