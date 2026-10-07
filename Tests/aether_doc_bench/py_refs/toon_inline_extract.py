import json
doc = json.loads('{"name":"Aether","score":42}')
print(f"{doc['name']} {doc['score']}")
