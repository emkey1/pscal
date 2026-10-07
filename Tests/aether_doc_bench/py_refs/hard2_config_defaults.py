import json
d = json.loads('{"services":[{"name":"api","port":8080,"tls":true,"retries":3},{"name":"worker","tls":false},{"name":"cache","port":6379},{"name":"edge","port":443,"tls":true}]}')
dp = te = 0
for s in d["services"]:
    if "port" in s:
        port = s["port"]
    else:
        port = 9000; dp += 1
    tls = s.get("tls", False); r = s.get("retries", 1)
    if tls:
        te += 1
    print(f"svc {s['name']}: port={port} tls={'true' if tls else 'false'} retries={r}")
print(f"defaulted ports = {dp}")
print(f"tls enabled = {te}")
print(f"services = {len(d['services'])}")
