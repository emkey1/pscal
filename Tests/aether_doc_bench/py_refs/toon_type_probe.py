import json
doc = json.loads('{"values":[42, "hi", true, 3.5]}')
def kind(v):
    if isinstance(v, bool): return "bool"
    if isinstance(v, int): return "int"
    if isinstance(v, float): return "real"
    if isinstance(v, str): return "text"
    return "other"
for i, v in enumerate(doc["values"]):
    print(f"item {i}: {kind(v)}")
