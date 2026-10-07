import io
s = io.BytesIO("id=17;name=widget;qty=240".encode())
body = s.getvalue().decode()
s.close()
fields = body.split(";")
kv = {}
for f in fields:
    k, _, v = f.partition("=")
    kv[k] = v
print(f"bytes={len(body)} fields={len(fields)}")
print(f"name={kv['name']} qty={kv['qty']} doubled={int(kv['qty'])*2}")
