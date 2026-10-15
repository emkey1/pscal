import json, urllib.request, os, sys, shutil
URL = "http://localhost:8019/v1/chat/completions"
W = "$HOME/training/aether-qwen-coder-30b-unsloth"
IN = sys.argv[1] if len(sys.argv) > 1 else "data_qwen_ml1x"
OUT = sys.argv[2] if len(sys.argv) > 2 else "data_qwen_ml1x_think"
recs = [json.loads(l) for l in open(W + "/" + IN + "/aether_instruction_sft.jsonl")]
od = W + "/" + OUT
os.makedirs(od, exist_ok=True)

def trace_for(task, code):
    p = ("Here is a programming task and a correct solution in Aether (a compact language: "
         "fn/let/ret, fx{ } wraps all output, half-open loops loop i in 0..n, println concatenates "
         "with no separator).\n\nTASK:\n" + task + "\n\nCORRECT SOLUTION:\n" + code + "\n\n"
         "Write a brief first-person plan (3-6 sentences) of how to arrive at this solution: the "
         "types/helpers, the control flow, and any Aether-specific care. Output ONLY the plan, no "
         "code, no preamble.")
    body = json.dumps({"model": "qwen3-8b-base", "temperature": 0.7, "max_tokens": 500,
                       "messages": [{"role": "user", "content": p}],
                       "chat_template_kwargs": {"enable_thinking": False}}).encode()
    req = urllib.request.Request(URL, data=body, headers={"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=120))["choices"][0]["message"]["content"].strip()

o = open(od + "/aether_instruction_sft.jsonl", "w")
n = 0
for i, rec in enumerate(recs):
    m = rec["messages"]
    task = m[1]["content"]
    code = m[-1]["content"]
    try:
        tr = trace_for(task, code)
    except Exception:
        tr = "Identify the types and helpers, set up main with fx-wrapped output, handle the loop and formatting precisely."
    if not tr or len(tr) < 10:
        tr = "Plan the types and helpers, then write main with fx-wrapped output and exact formatting."
    nr = dict(rec)
    nr["messages"] = [m[0], m[1], {"role": "assistant", "content": "<think>\n" + tr + "\n</think>\n\n" + code}]
    o.write(json.dumps(nr) + "\n")
    n += 1
    if i % 50 == 0:
        print("  %d/%d" % (i, len(recs)), flush=True)
o.close()
shutil.copy(W + "/" + IN + "/aether_repair_sft.jsonl", od + "/aether_repair_sft.jsonl")
print("TRACES_DONE wrote %d records to %s" % (n, OUT))
