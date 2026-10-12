#!/usr/bin/env python3
"""Compare guide variants on the simple suite, case by case (task and seed matched).

usage: compare.py NAME=REPORT.json [NAME=REPORT.json ...]     (.json or .json.gz)

Per variant: first-attempt passes (FA), passes after repair (FX), silent-wrong first
attempts (exit 0, wrong output), coded and uncoded errors, cases not sent (did not fit
the window), with a Wilson 95% interval on FA. Then one row per task (first-attempt
passes out of the repeats, per variant) and the most common first errors per variant.
"""
import collections, gzip, json, math, re, sys

def load(path):
    with (gzip.open if path.endswith(".gz") else open)(path, "rt") as f:
        r = json.load(f)
    return r["destinations"][0]["variants"][0]["results"]

def classify(case):
    a0 = case["attempts"][0] if case.get("attempts") else case
    run = a0.get("run") or {}
    if run.get("exact_stdout_match"):
        return "pass", ""
    if not run:
        return "not_sent", ""
    err = (run.get("stderr") or "").strip()
    if run.get("returncode") == 0:
        return "silent_wrong", ""
    first = next((l for l in err.splitlines() if l.strip()), "")
    first = re.sub(r"^\S+?:\d+: ", "", first)
    m = re.search(r"\[([A-Z]+-\d+)\]", err)
    return ("coded" if m else "uncoded"), first[:95]

def wilson(k, n):
    if n == 0:
        return (0.0, 0.0)
    z = 1.96; p = k / n; d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d; h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (100 * (c - h), 100 * (c + h))

names, data = [], {}
for spec in sys.argv[1:]:
    name, path = spec.split("=", 1)
    names.append(name); data[name] = load(path)

print("| variant | cases | FA | FA 95% | FX | silent-wrong | coded | uncoded | not sent | prompt tokens (median) |")
print("|---|---|---|---|---|---|---|---|---|---|")
tasks, per = [], {}
for name in names:
    cnt = collections.Counter(); fx = 0; toks = []
    for c in data[name]:
        k, _ = classify(c); cnt[k] += 1
        fx += bool((c.get("run") or {}).get("exact_stdout_match"))
        a0 = c["attempts"][0] if c.get("attempts") else c
        if a0.get("prompt_tokens_measured"): toks.append(a0["prompt_tokens_measured"])
        if c["task_id"] not in tasks: tasks.append(c["task_id"])
        per.setdefault(c["task_id"], {}).setdefault(name, []).append(k)
    n = len(data[name]); lo, hi = wilson(cnt["pass"], n)
    med = sorted(toks)[len(toks) // 2] if toks else "-"
    print(f"| {name} | {n} | {cnt['pass']} ({100 * cnt['pass'] / max(n, 1):.0f}%) | {lo:.0f}-{hi:.0f}% | {fx} | {cnt['silent_wrong']} | {cnt['coded']} | {cnt['uncoded']} | {cnt['not_sent']} | {med} |")

print("\n| task | " + " | ".join(names) + " |")
print("|---|" + "---|" * len(names))
mark = {"pass": "P", "silent_wrong": "w", "coded": "e", "uncoded": "u", "not_sent": "-"}
for t in tasks:
    print(f"| {t} | " + " | ".join("".join(mark[k] for k in per[t].get(n, [])) for n in names) + " |")
print("\nP pass, w silent-wrong, e coded error, u uncoded error, - not sent; one letter per repeat.")

for name in names:
    errs = collections.Counter(e for c in data[name] for k, e in [classify(c)] if e)
    print(f"\nfirst errors, {name}:")
    for e, n in errs.most_common(8):
        print(f"  {n:3d}  {e}")
