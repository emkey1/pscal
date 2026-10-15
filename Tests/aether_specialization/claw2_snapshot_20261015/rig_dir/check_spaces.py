import json, urllib.request
URL="http://localhost:8019/v1/chat/completions"
SYS="You generate canonical Aether. When asked for code, output raw Aether source only."
PROMPTS={
 "hello_fx":"Write the smallest valid Aether program that prints exactly `hello from benchmark` followed by a newline.",
 "range_loop_squares":"Write an Aether program that uses a `loop i in 0..5` style range loop and prints one line per iteration in the form `0 => 0`, `1 => 1`, ..., `4 => 16`.",
}
for tid,user in PROMPTS.items():
    body=json.dumps({"model":"mistral24b-ml1x","temperature":0,"max_tokens":256,"messages":[{"role":"system","content":SYS},{"role":"user","content":user}]}).encode()
    req=urllib.request.Request(URL,data=body,headers={"Content-Type":"application/json"})
    c=json.load(urllib.request.urlopen(req,timeout=120))["choices"][0]["message"]["content"]
    has=("hello from benchmark" in c) or (" => " in c)
    print("TASK", tid, "SPACES_OK" if has else "SPACES_BAD")
    print(repr(c[:200]))
