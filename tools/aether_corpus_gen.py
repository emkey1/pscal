#!/usr/bin/env python3
"""Generate verified compositional Aether corpus candidates from parameterized
templates, each with a Python oracle. A candidate is KEPT only if its compiled
output matches the oracle byte-for-byte, so a buggy emission can never enter the
corpus (it would self-certify only if its output happened to equal the true
answer). This grows the corpus with the longer, multi-function programs the
short benchmark-shaped corpus lacks."""
import random, subprocess, pathlib, sys

AETHER = "/Users/mke/PBuild/build/bin/aether"
CORPUS = pathlib.Path("/Users/mke/PBuild/Tests/aether_specialization/corpus_candidates")

def f2(x):  # 2-decimal money, matches Aether :0:2
    return f"{x:.2f}"

# ---- template: running bank ledger (deposit / overdraft-guarded withdraw / interest) ----
def ledger(seed):
    rng = random.Random(seed)
    bal = float(rng.choice([100, 250, 500]))
    ops = []
    for _ in range(rng.randint(8, 12)):
        op = rng.choices(["deposit", "withdraw", "interest"], weights=[4, 4, 2])[0]
        ops.append((op, rng.choice([0.01, 0.02, 0.05]) if op == "interest" else float(rng.randint(10, 300))))
    exp = [f"Starting balance: ${f2(bal)}"]
    body = [f"    let balance: Real = {bal};",
            '    fx { println("Starting balance: $", balance:0:2); }']
    for op, val in ops:
        if op == "deposit":
            bal += val
            exp.append(f"Deposit ${f2(val)}: ${f2(bal)}")
            body += [f"    balance = deposit(balance, {val});",
                     f'    fx {{ println("Deposit ${f2(val)}: $", balance:0:2); }}']
        elif op == "withdraw":
            if bal >= val:
                bal -= val
            exp.append(f"Withdraw ${f2(val)}: ${f2(bal)}")
            body += [f"    balance = withdraw(balance, {val});",
                     f'    fx {{ println("Withdraw ${f2(val)}: $", balance:0:2); }}']
        else:
            bal *= (1.0 + val)
            pct = f"{val * 100:.0f}"
            exp.append(f"Interest {pct}%: ${f2(bal)}")
            body += [f"    balance = applyInterest(balance, {val});",
                     f'    fx {{ println("Interest {pct}%: $", balance:0:2); }}']
    hdr = ("// {name}\n"
           "// Concepts: Real arithmetic, helper functions, overdraft guard, percentage interest, "
           "fx formatted output, multi-step state\n"
           + "".join(f"// expect: {l}\n" for l in exp))
    prog = ("\nfn deposit(balance: Real, amount: Real) -> Real {\n    ret balance + amount;\n}\n\n"
            "fn withdraw(balance: Real, amount: Real) -> Real {\n"
            "    if balance >= amount { ret balance - amount; }\n    ret balance;\n}\n\n"
            "fn applyInterest(balance: Real, rate: Real) -> Real {\n    ret balance * (1.0 + rate);\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(exp)

# ---- template: gradebook (N students, K scores each, per-student average + pass/fail, class average) ----
def gradebook(seed):
    rng = random.Random(seed)
    NAMES = ["Ana", "Ben", "Cy", "Dee", "Eli", "Fay", "Gus", "Hana", "Ivy", "Jed",
             "Kai", "Lia", "Mo", "Nia", "Omar", "Pia", "Quin", "Rex", "Sam", "Tess"]
    n = rng.randint(4, 6)
    k = rng.choice([3, 4])
    names = rng.sample(NAMES, n)
    scores = [[rng.randint(35, 100) for _ in range(k)] for _ in range(n)]
    # oracle
    exp = []
    class_sum = 0
    for i in range(n):
        ssum = sum(scores[i])
        avg = ssum * 1.0 / k
        status = "pass" if ssum >= 60 * k else "fail"  # avg >= 60 <=> sum >= 60*k
        exp.append(f"{names[i]}: avg {f2(avg)} ({status})")
        class_sum += ssum
    class_avg = class_sum * 1.0 / (n * k)
    exp.append(f"class average: {f2(class_avg)}")
    # emit
    flat = []
    for row in scores:
        flat += row
    body = [f"    let names: Text[] = [{', '.join(chr(34)+x+chr(34) for x in names)}];",
            f"    let scores: Int[] = [{', '.join(str(x) for x in flat)}];",
            f"    let students: Int = {n};",
            f"    let perStudent: Int = {k};",
            "    let classSum: Int = 0;",
            "    loop s in 0..students {",
            "        let base: Int = s * perStudent;",
            "        let total: Int = sumScores(scores, base, perStudent);",
            "        classSum = classSum + total;",
            "        let avg: Real = total * 1.0 / perStudent;",
            "        let status: Text = grade(total, perStudent);",
            '        fx { println(names[s], ": avg ", avg:0:2, " (", status, ")"); }',
            "    }",
            "    let classAvg: Real = classSum * 1.0 / (students * perStudent);",
            '    fx { println("class average: ", classAvg:0:2); }']
    hdr = ("// {name}\n"
           "// Concepts: parallel Text/Int arrays, flat per-student score blocks, helper functions, "
           "indexed accumulation, real average, pass/fail threshold, class rollup\n"
           + "".join(f"// expect: {l}\n" for l in exp))
    prog = ("\nfn sumScores(scores: Int[], base: Int, count: Int) -> Int {\n"
            "    let total: Int = 0;\n"
            "    loop j in 0..count {\n        total = total + scores[base + j];\n    }\n    ret total;\n}\n\n"
            "fn grade(total: Int, count: Int) -> Text {\n"
            "    if total >= 60 * count { ret \"pass\"; }\n    ret \"fail\";\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(exp)

# ---- template: inventory (N products, qty + unit price, line totals, grand total, low-stock flag) ----
def inventory(seed):
    rng = random.Random(seed)
    NAMES = ["Bolt", "Nut", "Washer", "Gear", "Spring", "Cable", "Valve", "Pump",
             "Hinge", "Bracket", "Screw", "Plate", "Rod", "Clamp", "Seal", "Bearing"]
    n = rng.randint(9, 13)
    names = rng.sample(NAMES, n)
    qtys = [rng.randint(0, 40) for _ in range(n)]
    # prices that are exact in 2 decimals (cents), as float
    prices = [round(rng.randint(50, 4000) / 100.0, 2) for _ in range(n)]
    thr = rng.choice([5, 8, 10])
    # oracle
    exp = []
    grand = 0.0
    low = 0
    for i in range(n):
        line = qtys[i] * prices[i]
        grand += line
        flag = " LOW" if qtys[i] < thr else ""
        exp.append(f"{names[i]}: {qtys[i]} x ${f2(prices[i])} = ${f2(line)}{flag}")
        if qtys[i] < thr:
            low += 1
    exp.append(f"grand total: ${f2(grand)}")
    exp.append(f"low-stock items: {low}")
    # emit
    body = [f"    let names: Text[] = [{', '.join(chr(34)+x+chr(34) for x in names)}];",
            f"    let qtys: Int[] = [{', '.join(str(x) for x in qtys)}];",
            f"    let prices: Real[] = [{', '.join(f2(x) for x in prices)}];",
            f"    let threshold: Int = {thr};",
            "    let n: Int = length(qtys);",
            "    let grand: Real = 0.0;",
            "    let low: Int = 0;",
            "    loop i in 0..n {",
            "        let line: Real = qtys[i] * prices[i];",
            "        grand = grand + line;",
            '        let flag: Text = lowFlag(qtys[i], threshold);',
            "        if qtys[i] < threshold { low = low + 1; }",
            '        fx { println(names[i], ": ", qtys[i], " x $", prices[i]:0:2, " = $", line:0:2, flag); }',
            "    }",
            '    fx { println("grand total: $", grand:0:2); }',
            '    fx { println("low-stock items: ", low); }']
    hdr = ("// {name}\n"
           "// Concepts: parallel Text/Int/Real arrays, Int*Real line totals, running real grand total, "
           "threshold flag helper, formatted currency output, summary counts\n"
           + "".join(f"// expect: {l}\n" for l in exp))
    prog = ("\nfn lowFlag(qty: Int, threshold: Int) -> Text {\n"
            "    if qty < threshold { ret \" LOW\"; }\n    ret \"\";\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(exp)

# ---- template: stats_summary (Int[] readings -> count/sum/min/max/mean/above-mean) ----
def stats_summary(seed):
    rng = random.Random(seed)
    n = rng.randint(6, 10)
    xs = [rng.randint(1, 99) for _ in range(n)]
    # oracle
    total = sum(xs)
    mn = min(xs)
    mx = max(xs)
    mean = total * 1.0 / n
    above = sum(1 for v in xs if v > mean)
    exp = [f"count: {n}",
           f"sum: {total}",
           f"min: {mn}",
           f"max: {mx}",
           f"mean: {f2(mean)}",
           f"above mean: {above}"]
    # emit
    body = [f"    let xs: Int[] = [{', '.join(str(x) for x in xs)}];",
            "    let n: Int = length(xs);",
            "    let total: Int = sumOf(xs);",
            "    let lo: Int = minOf(xs);",
            "    let hi: Int = maxOf(xs);",
            "    let mean: Real = total * 1.0 / n;",
            "    let above: Int = 0;",
            "    loop i in 0..n {",
            "        if xs[i] * 1.0 > mean { above = above + 1; }",
            "    }",
            "    fx {",
            '        println("count: ", n);',
            '        println("sum: ", total);',
            '        println("min: ", lo);',
            '        println("max: ", hi);',
            '        println("mean: ", mean:0:2);',
            '        println("above mean: ", above);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: Int[] readings, reduction helpers (sum/min/max), real mean, "
           "second pass counting values above the mean, fx summary block\n"
           + "".join(f"// expect: {l}\n" for l in exp))
    prog = ("\nfn sumOf(xs: Int[]) -> Int {\n    let total: Int = 0;\n"
            "    loop i in 0..length(xs) {\n        total = total + xs[i];\n    }\n    ret total;\n}\n\n"
            "fn minOf(xs: Int[]) -> Int {\n    let m: Int = xs[0];\n"
            "    loop i in 0..length(xs) {\n        if xs[i] < m { m = xs[i]; }\n    }\n    ret m;\n}\n\n"
            "fn maxOf(xs: Int[]) -> Int {\n    let m: Int = xs[0];\n"
            "    loop i in 0..length(xs) {\n        if xs[i] > m { m = xs[i]; }\n    }\n    ret m;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(exp)

# ---- template: temperature_log (Real[] readings clamped into range, min/max/avg summary) ----
def temperature_log(seed):
    rng = random.Random(seed)
    n = rng.randint(6, 9)
    lo = float(rng.choice([0, 5, 10]))
    hi = float(rng.choice([30, 35, 40]))
    # raw readings, some outside [lo,hi], all exact 1-decimal so float is clean
    raw = [round(rng.randint(int(lo) * 10 - 80, int(hi) * 10 + 80) / 10.0, 1) for _ in range(n)]
    # oracle
    clamped = [min(max(r, lo), hi) for r in raw]
    exp = []
    adjusted = 0
    for i in range(n):
        tag = " (clamped)" if clamped[i] != raw[i] else ""
        exp.append(f"reading {i}: {f2(clamped[i])}{tag}")
        if clamped[i] != raw[i]:
            adjusted += 1
    cmin = min(clamped)
    cmax = max(clamped)
    cavg = sum(clamped) / n
    exp.append(f"min: {f2(cmin)}")
    exp.append(f"max: {f2(cmax)}")
    exp.append(f"avg: {f2(cavg)}")
    exp.append(f"clamped readings: {adjusted}")
    # emit
    body = [f"    let raw: Real[] = [{', '.join(f'{x:.1f}' for x in raw)}];",
            f"    let lo: Real = {lo:.1f};",
            f"    let hi: Real = {hi:.1f};",
            "    let n: Int = length(raw);",
            "    let sum: Real = 0.0;",
            "    let adjusted: Int = 0;",
            "    let cmin: Real = clamp(raw[0], lo, hi);",
            "    let cmax: Real = clamp(raw[0], lo, hi);",
            "    loop i in 0..n {",
            "        let c: Real = clamp(raw[i], lo, hi);",
            "        sum = sum + c;",
            "        if c < cmin { cmin = c; }",
            "        if c > cmax { cmax = c; }",
            "        let tag: Text = clampTag(raw[i], c);",
            "        if c != raw[i] { adjusted = adjusted + 1; }",
            '        fx { println("reading ", i, ": ", c:0:2, tag); }',
            "    }",
            "    let avg: Real = sum / n;",
            "    fx {",
            '        println("min: ", cmin:0:2);',
            '        println("max: ", cmax:0:2);',
            '        println("avg: ", avg:0:2);',
            '        println("clamped readings: ", adjusted);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: Real[] readings, clamp(x,lo,hi) builtin into a valid range, "
           "per-reading clamp flag helper, running min/max/sum over clamped values, real average, summary\n"
           + "".join(f"// expect: {l}\n" for l in exp))
    prog = ("\nfn clampTag(raw: Real, clamped: Real) -> Text {\n"
            "    if clamped != raw { ret \" (clamped)\"; }\n    ret \"\";\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(exp)

# ---- template: category_counter (items tagged with a category -> count per category) ----
def category_counter(seed):
    rng = random.Random(seed)
    CATS = ["fruit", "veg", "grain", "dairy", "meat"]
    cats = rng.sample(CATS, rng.randint(3, 4))
    ITEMS = {
        "fruit": ["apple", "pear", "plum", "fig", "lime"],
        "veg": ["kale", "leek", "bean", "corn", "pea"],
        "grain": ["rice", "oat", "rye", "wheat", "barley"],
        "dairy": ["milk", "cheese", "yogurt", "butter", "cream"],
        "meat": ["beef", "pork", "lamb", "duck", "ham"],
    }
    nitems = rng.randint(8, 14)
    seq = [rng.choice(cats) for _ in range(nitems)]
    items = [rng.choice(ITEMS[c]) for c in seq]
    # oracle: counts reported in first-appearance order of categories (stable)
    order = []
    counts = {}
    for c in seq:
        if c not in counts:
            counts[c] = 0
            order.append(c)
        counts[c] += 1
    exp = [f"item {i}: {items[i]} [{seq[i]}]" for i in range(nitems)]
    for c in order:
        exp.append(f"{c}: {counts[c]}")
    exp.append(f"categories: {len(order)}")
    # emit: parallel arrays; tally with a known category list, counted in first-appearance order
    body = [f"    let items: Text[] = [{', '.join(chr(34)+x+chr(34) for x in items)}];",
            f"    let tags: Text[] = [{', '.join(chr(34)+x+chr(34) for x in seq)}];",
            f"    let cats: Text[] = [{', '.join(chr(34)+x+chr(34) for x in order)}];",
            "    let n: Int = length(tags);",
            "    let c: Int = length(cats);",
            "    loop i in 0..n {",
            '        fx { println("item ", i, ": ", items[i], " [", tags[i], "]"); }',
            "    }",
            "    loop j in 0..c {",
            "        let total: Int = countTag(tags, cats[j]);",
            '        fx { println(cats[j], ": ", total); }',
            "    }",
            '    fx { println("categories: ", c); }']
    hdr = ("// {name}\n"
           "// Concepts: parallel Text arrays of items and category tags, per-category tally helper "
           "with string equality, first-appearance ordering, counted summary\n"
           + "".join(f"// expect: {l}\n" for l in exp))
    prog = ("\nfn countTag(tags: Text[], cat: Text) -> Int {\n    let total: Int = 0;\n"
            "    loop i in 0..length(tags) {\n        if tags[i] == cat { total = total + 1; }\n    }\n    ret total;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(exp)

def verify(name, hdr, prog, expect):
    src = hdr.replace("{name}", name) + prog
    tmp = f"/tmp/{name}.aether"
    pathlib.Path(tmp).write_text(src)
    r = subprocess.run([AETHER, "--no-cache", tmp], capture_output=True, text=True, timeout=30)
    return (r.returncode == 0 and r.stdout.rstrip("\n") == expect.rstrip("\n")), src, r

def run_template(prefix, fn, target, seed0):
    kept = tried = totlines = 0
    seed = seed0
    while kept < target and tried < target * 4:
        tried += 1
        hdr, prog, expect = fn(seed); seed += 1
        name = f"{prefix}_{kept:03d}"
        ok, src, r = verify(name, hdr, prog, expect)
        if ok:
            (CORPUS / name).write_text(src); kept += 1; totlines += src.count("\n")
        elif tried <= 2:
            print(f"  [{prefix}] miss rc={r.returncode} match={r.stdout.rstrip(chr(10))==expect.rstrip(chr(10))}")
            print("   GOT:", repr(r.stdout[:120]), "\n   EXP:", repr(expect[:120]), "\n   ERR:", r.stderr[:120])
    print(f"{prefix}: kept {kept}/{tried} attempts, avg {totlines//max(kept,1)} lines")
    return kept

if __name__ == "__main__":
    import os
    only = sys.argv[1] if len(sys.argv) > 1 else None
    plan = [
        ("301_gradebook", gradebook, 32, 2000),
        ("302_inventory", inventory, 32, 3000),
        ("303_stats_summary", stats_summary, 32, 4000),
        ("304_temperature_log", temperature_log, 32, 5000),
        ("305_category_counter", category_counter, 32, 6000),
    ]
    total = 0
    for prefix, fn, target, seed0 in plan:
        if only and only not in prefix:
            continue
        total += run_template(prefix, fn, target, seed0)
    print(f"TOTAL new kept: {total}")
