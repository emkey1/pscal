#!/usr/bin/env python3
"""Generate verified compositional Aether corpus candidates from parameterized
templates, each with a Python oracle. A candidate is KEPT only if its compiled
output matches the oracle byte-for-byte, so a buggy emission can never enter the
corpus (it would self-certify only if its output happened to equal the true
answer). This grows the corpus with the longer, multi-function programs the
short benchmark-shaped corpus lacks."""
import random, subprocess, pathlib, sys

AETHER = "/Users/mke/PBuild/components/aether/build/aether"
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

# ===================== CS-classics templates (306+) =====================
# Each teaches a classic algorithm PATTERN with DECONTAMINATED specific
# problems (no factorial/fibonacci/gcd/fizzbuzz/sieve/collatz/hanoi/nqueens/
# bubble_sort/merge_sort/quick_sort/bfs/dijkstra/coin_change/lis/lcs/
# edit_distance/substring). Seeded inputs make outputs differ from fixed tests.

# ---- template: recursion drills (power, digit-sum, count-digits, sum-1..n, reverse-int) ----
def recursion_drills(seed):
    rng = random.Random(seed)
    base = rng.randint(2, 7)
    exp = rng.randint(3, 9)
    dnum = rng.randint(1000, 999999)
    cnum = rng.randint(10, 9999999)
    lim = rng.randint(5, 40)
    rnum = rng.randint(100, 9999999)
    # oracle
    def power(b, e):
        if e == 0:
            return 1
        return b * power(b, e - 1)
    def digit_sum(n):
        if n == 0:
            return 0
        return (n % 10) + digit_sum(n // 10)
    def count_digits(n):
        if n < 10:
            return 1
        return 1 + count_digits(n // 10)
    def sum_to(n):
        if n == 0:
            return 0
        return n + sum_to(n - 1)
    def rev_int(n, acc):
        if n == 0:
            return acc
        return rev_int(n // 10, acc * 10 + n % 10)
    pw = power(base, exp)
    ds = digit_sum(dnum)
    cd = count_digits(cnum)
    st = sum_to(lim)
    ri = rev_int(rnum, 0)
    out = [f"power({base}, {exp}) = {pw}",
           f"digit_sum({dnum}) = {ds}",
           f"count_digits({cnum}) = {cd}",
           f"sum_to({lim}) = {st}",
           f"reverse_int({rnum}) = {ri}"]
    body = [f"    let pw: Int = power({base}, {exp});",
            f"    let ds: Int = digitSum({dnum});",
            f"    let cd: Int = countDigits({cnum});",
            f"    let st: Int = sumTo({lim});",
            f"    let ri: Int = reverseInt({rnum}, 0);",
            "    fx {",
            f'        println("power({base}, {exp}) = ", pw);',
            f'        println("digit_sum({dnum}) = ", ds);',
            f'        println("count_digits({cnum}) = ", cd);',
            f'        println("sum_to({lim}) = ", st);',
            f'        println("reverse_int({rnum}) = ", ri);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: classic single-recursion drills (exponentiation, digit reduction, "
           "digit count, triangular sum, integer reversal with accumulator), Int %/ division, base cases\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn power(base: Int, exp: Int) -> Int {\n"
            "    if exp == 0 { ret 1; }\n    ret base * power(base, exp - 1);\n}\n\n"
            "fn digitSum(n: Int) -> Int {\n"
            "    if n == 0 { ret 0; }\n    ret (n % 10) + digitSum(n / 10);\n}\n\n"
            "fn countDigits(n: Int) -> Int {\n"
            "    if n < 10 { ret 1; }\n    ret 1 + countDigits(n / 10);\n}\n\n"
            "fn sumTo(n: Int) -> Int {\n"
            "    if n == 0 { ret 0; }\n    ret n + sumTo(n - 1);\n}\n\n"
            "fn reverseInt(n: Int, acc: Int) -> Int {\n"
            "    if n == 0 { ret acc; }\n    ret reverseInt(n / 10, acc * 10 + n % 10);\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

# ---- template: sorting (insertion sort OR selection sort on a seeded Int[]) ----
def sorting_drills(seed):
    rng = random.Random(seed)
    n = rng.randint(5, 10)
    xs = [rng.randint(1, 99) for _ in range(n)]
    algo = rng.choice(["insertion", "selection"])
    srt = sorted(xs)
    out = [f"input: {' '.join(str(v) for v in xs)}",
           f"algorithm: {algo}"]
    for i in range(n):
        out.append(f"sorted[{i}] = {srt[i]}")
    out.append(f"min: {srt[0]}")
    out.append(f"max: {srt[n - 1]}")
    arr_lit = ", ".join(str(v) for v in xs)
    if algo == "insertion":
        sortcall = "    xs = insertionSort(xs);"
    else:
        sortcall = "    xs = selectionSort(xs);"
    body = [f"    let xs: Int[] = [{arr_lit}];",
            f'    let algo: Text = "{algo}";',
            "    let n: Int = length(xs);",
            "    fx { println(\"input: \", joinInts(xs)); }",
            '    fx { println("algorithm: ", algo); }',
            sortcall,
            "    loop i in 0..n {",
            '        fx { println("sorted[", i, "] = ", xs[i]); }',
            "    }",
            "    fx {",
            '        println("min: ", xs[0]);',
            '        println("max: ", xs[n - 1]);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: comparison sorting (insertion sort shifts a key left into place; "
           "selection sort swaps the running minimum forward), in-place Int[] mutation, "
           "indexed element output, min/max of the sorted array\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn insertionSort(xs: Int[]) -> Int[] {\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 1..n {\n"
            "        let key: Int = xs[i];\n"
            "        let j: Int = i - 1;\n"
            "        loop j >= 0 {\n"
            "            if xs[j] <= key { break; }\n"
            "            xs[j + 1] = xs[j];\n"
            "            j = j - 1;\n"
            "        }\n"
            "        xs[j + 1] = key;\n"
            "    }\n"
            "    ret xs;\n}\n\n"
            "fn selectionSort(xs: Int[]) -> Int[] {\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 0..n {\n"
            "        let m: Int = i;\n"
            "        loop j in i + 1..n {\n"
            "            if xs[j] < xs[m] { m = j; }\n"
            "        }\n"
            "        let tmp: Int = xs[i];\n"
            "        xs[i] = xs[m];\n"
            "        xs[m] = tmp;\n"
            "    }\n"
            "    ret xs;\n}\n\n"
            "fn joinInts(xs: Int[]) -> Text {\n"
            "    let out: Text = \"\";\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 0..n {\n"
            "        if i > 0 { out = out + \" \"; }\n"
            "        out = out + int_to_text(xs[i]);\n"
            "    }\n"
            "    ret out;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

# ---- template: searching (linear index-of, find-min-index, binary search on sorted) ----
def searching_drills(seed):
    rng = random.Random(seed)
    n = rng.randint(6, 11)
    xs = [rng.randint(1, 80) for _ in range(n)]
    # linear target: sometimes present, sometimes absent
    if rng.random() < 0.6:
        ltgt = rng.choice(xs)
    else:
        ltgt = rng.randint(81, 99)  # guaranteed absent
    # sorted array for binary search (distinct values, varied targets)
    svals = sorted(rng.sample(range(1, 100), n))
    if rng.random() < 0.6:
        btgt = rng.choice(svals)
    else:
        cand = rng.randint(1, 99)
        while cand in svals:
            cand = rng.randint(1, 99)
        btgt = cand
    # oracle: linear index-of (first match) or -1
    lin = -1
    for i, v in enumerate(xs):
        if v == ltgt:
            lin = i
            break
    # find-min-index (first occurrence of the minimum)
    mi = 0
    for i in range(1, n):
        if xs[i] < xs[mi]:
            mi = i
    # binary search index or -1
    lo, hi = 0, n - 1
    bin_idx = -1
    while lo <= hi:
        mid = (lo + hi) // 2
        if svals[mid] == btgt:
            bin_idx = mid
            break
        if svals[mid] < btgt:
            lo = mid + 1
        else:
            hi = mid - 1
    out = [f"array: {' '.join(str(v) for v in xs)}",
           f"sorted: {' '.join(str(v) for v in svals)}",
           f"index_of({ltgt}) = {lin}",
           f"min_index = {mi}",
           f"min_value = {xs[mi]}",
           f"binary_search({btgt}) = {bin_idx}"]
    body = [f"    let xs: Int[] = [{', '.join(str(v) for v in xs)}];",
            f"    let sorted: Int[] = [{', '.join(str(v) for v in svals)}];",
            f"    let ltgt: Int = {ltgt};",
            f"    let btgt: Int = {btgt};",
            "    let lin: Int = indexOf(xs, ltgt);",
            "    let mi: Int = minIndex(xs);",
            "    let bin: Int = binarySearch(sorted, btgt);",
            "    fx {",
            '        println("array: ", joinInts(xs));',
            '        println("sorted: ", joinInts(sorted));',
            '        println("index_of(", ltgt, ") = ", lin);',
            '        println("min_index = ", mi);',
            '        println("min_value = ", xs[mi]);',
            '        println("binary_search(", btgt, ") = ", bin);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: linear search returning first matching index or -1, "
           "find-min-index scan, binary search over a sorted Int[] (lo/hi/mid bisection), "
           "present and absent targets\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn indexOf(xs: Int[], target: Int) -> Int {\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 0..n {\n"
            "        if xs[i] == target { ret i; }\n"
            "    }\n    ret -1;\n}\n\n"
            "fn minIndex(xs: Int[]) -> Int {\n"
            "    let m: Int = 0;\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 1..n {\n"
            "        if xs[i] < xs[m] { m = i; }\n"
            "    }\n    ret m;\n}\n\n"
            "fn binarySearch(xs: Int[], target: Int) -> Int {\n"
            "    let lo: Int = 0;\n"
            "    let hi: Int = length(xs) - 1;\n"
            "    loop lo <= hi {\n"
            "        let mid: Int = (lo + hi) / 2;\n"
            "        if xs[mid] == target { ret mid; }\n"
            "        if xs[mid] < target { lo = mid + 1; }\n"
            "        if xs[mid] > target { hi = mid - 1; }\n"
            "    }\n    ret -1;\n}\n\n"
            "fn joinInts(xs: Int[]) -> Text {\n"
            "    let out: Text = \"\";\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 0..n {\n"
            "        if i > 0 { out = out + \" \"; }\n"
            "        out = out + int_to_text(xs[i]);\n"
            "    }\n    ret out;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

# ---- template: DP-1D (Kadane max-subarray, climbing-stairs ways, house-robber max) ----
def dp1d_drills(seed):
    rng = random.Random(seed)
    n = rng.randint(6, 10)
    # mixed-sign array for Kadane (ensure at least one positive)
    arr = [rng.randint(-9, 9) for _ in range(n)]
    if max(arr) < 0:
        arr[rng.randrange(n)] = rng.randint(1, 9)
    stairs = rng.randint(4, 18)
    m = rng.randint(5, 9)
    houses = [rng.randint(1, 30) for _ in range(m)]
    # oracle: Kadane
    best = arr[0]
    cur = arr[0]
    for i in range(1, n):
        if cur + arr[i] > arr[i]:
            cur = cur + arr[i]
        else:
            cur = arr[i]
        if cur > best:
            best = cur
    # climbing stairs (1 or 2 steps): ways[k]=ways[k-1]+ways[k-2], ways[0]=1, ways[1]=1
    a, b = 1, 1
    for _ in range(2, stairs + 1):
        a, b = b, a + b
    ways = b if stairs >= 1 else 1
    # house robber: rob[i] = max(rob[i-1], rob[i-2]+val)
    prev2, prev1 = 0, 0
    for v in houses:
        take = prev2 + v
        skip = prev1
        cur_r = take if take > skip else skip
        prev2, prev1 = prev1, cur_r
    rob = prev1
    out = [f"array: {' '.join(str(v) for v in arr)}",
           f"max_subarray = {best}",
           f"stairs({stairs}) ways = {ways}",
           f"houses: {' '.join(str(v) for v in houses)}",
           f"rob_max = {rob}"]
    body = [f"    let arr: Int[] = [{', '.join(str(v) for v in arr)}];",
            f"    let houses: Int[] = [{', '.join(str(v) for v in houses)}];",
            f"    let stairs: Int = {stairs};",
            "    let best: Int = maxSubarray(arr);",
            "    let ways: Int = climbStairs(stairs);",
            "    let rob: Int = robHouses(houses);",
            "    fx {",
            '        println("array: ", joinInts(arr));',
            '        println("max_subarray = ", best);',
            '        println("stairs(", stairs, ") ways = ", ways);',
            '        println("houses: ", joinInts(houses));',
            '        println("rob_max = ", rob);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: 1-D dynamic programming — Kadane's running max-subarray, "
           "climbing-stairs step counting (two-back recurrence), house-robber take/skip choice; "
           "rolling state, max() via if\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn maxSubarray(xs: Int[]) -> Int {\n"
            "    let best: Int = xs[0];\n"
            "    let cur: Int = xs[0];\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 1..n {\n"
            "        if cur + xs[i] > xs[i] { cur = cur + xs[i]; }\n"
            "        if cur + xs[i] <= xs[i] { cur = xs[i]; }\n"
            "        if cur > best { best = cur; }\n"
            "    }\n    ret best;\n}\n\n"
            "fn climbStairs(n: Int) -> Int {\n"
            "    let a: Int = 1;\n"
            "    let b: Int = 1;\n"
            "    loop i in 2..n + 1 {\n"
            "        let next: Int = a + b;\n"
            "        a = b;\n"
            "        b = next;\n"
            "    }\n    ret b;\n}\n\n"
            "fn robHouses(xs: Int[]) -> Int {\n"
            "    let prev2: Int = 0;\n"
            "    let prev1: Int = 0;\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 0..n {\n"
            "        let take: Int = prev2 + xs[i];\n"
            "        let best: Int = prev1;\n"
            "        if take > best { best = take; }\n"
            "        prev2 = prev1;\n"
            "        prev1 = best;\n"
            "    }\n    ret prev1;\n}\n\n"
            "fn joinInts(xs: Int[]) -> Text {\n"
            "    let out: Text = \"\";\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 0..n {\n"
            "        if i > 0 { out = out + \" \"; }\n"
            "        out = out + int_to_text(xs[i]);\n"
            "    }\n    ret out;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

# ---- template: DP-2D (grid unique-paths + min-path-sum; flat Int[] indexed r*cols+c) ----
def dp2d_drills(seed):
    rng = random.Random(seed)
    rows = rng.randint(2, 4)
    cols = rng.randint(2, 4)
    total = rows * cols
    cost = [rng.randint(1, 9) for _ in range(total)]
    # oracle: unique paths (down/right only)
    up = [0] * total
    for r in range(rows):
        for c in range(cols):
            idx = r * cols + c
            if r == 0 or c == 0:
                up[idx] = 1
            else:
                up[idx] = up[(r - 1) * cols + c] + up[r * cols + (c - 1)]
    paths = up[total - 1]
    # oracle: min path sum (down/right only)
    mp = [0] * total
    for r in range(rows):
        for c in range(cols):
            idx = r * cols + c
            if r == 0 and c == 0:
                mp[idx] = cost[idx]
            elif r == 0:
                mp[idx] = mp[idx - 1] + cost[idx]
            elif c == 0:
                mp[idx] = mp[(r - 1) * cols + c] + cost[idx]
            else:
                top = mp[(r - 1) * cols + c]
                left = mp[r * cols + (c - 1)]
                mp[idx] = (top if top < left else left) + cost[idx]
    minsum = mp[total - 1]
    out = [f"grid: {rows}x{cols}",
           f"cost: {' '.join(str(v) for v in cost)}",
           f"unique_paths = {paths}",
           f"min_path_sum = {minsum}"]
    body = [f"    let cost: Int[] = [{', '.join(str(v) for v in cost)}];",
            f"    let rows: Int = {rows};",
            f"    let cols: Int = {cols};",
            "    let paths: Int = uniquePaths(rows, cols);",
            "    let minsum: Int = minPathSum(cost, rows, cols);",
            "    fx {",
            '        println("grid: ", rows, "x", cols);',
            '        println("cost: ", joinInts(cost));',
            '        println("unique_paths = ", paths);',
            '        println("min_path_sum = ", minsum);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: 2-D grid dynamic programming flattened into an Int[] addressed r*cols+c — "
           "count down/right unique paths, and min-cost down/right path sum; boundary rows/cols, "
           "min() via if\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn uniquePaths(rows: Int, cols: Int) -> Int {\n"
            "    let total: Int = rows * cols;\n"
            "    let dp: Int[] = [];\n"
            "    loop i in 0..total {\n        dp = dp + [0];\n    }\n"
            "    loop r in 0..rows {\n"
            "        loop c in 0..cols {\n"
            "            let idx: Int = r * cols + c;\n"
            "            if r == 0 { dp[idx] = 1; }\n"
            "            if c == 0 { dp[idx] = 1; }\n"
            "            if r > 0 {\n"
            "                if c > 0 { dp[idx] = dp[(r - 1) * cols + c] + dp[r * cols + (c - 1)]; }\n"
            "            }\n"
            "        }\n    }\n"
            "    ret dp[total - 1];\n}\n\n"
            "fn minPathSum(cost: Int[], rows: Int, cols: Int) -> Int {\n"
            "    let total: Int = rows * cols;\n"
            "    let dp: Int[] = [];\n"
            "    loop i in 0..total {\n        dp = dp + [0];\n    }\n"
            "    loop r in 0..rows {\n"
            "        loop c in 0..cols {\n"
            "            let idx: Int = r * cols + c;\n"
            "            if r == 0 {\n"
            "                if c == 0 { dp[idx] = cost[idx]; }\n"
            "                if c > 0 { dp[idx] = dp[idx - 1] + cost[idx]; }\n"
            "            }\n"
            "            if r > 0 {\n"
            "                if c == 0 { dp[idx] = dp[(r - 1) * cols + c] + cost[idx]; }\n"
            "                if c > 0 {\n"
            "                    let top: Int = dp[(r - 1) * cols + c];\n"
            "                    let left: Int = dp[r * cols + (c - 1)];\n"
            "                    let best: Int = top;\n"
            "                    if left < best { best = left; }\n"
            "                    dp[idx] = best + cost[idx];\n"
            "                }\n"
            "            }\n"
            "        }\n    }\n"
            "    ret dp[total - 1];\n}\n\n"
            "fn joinInts(xs: Int[]) -> Text {\n"
            "    let out: Text = \"\";\n"
            "    let n: Int = length(xs);\n"
            "    loop i in 0..n {\n"
            "        if i > 0 { out = out + \" \"; }\n"
            "        out = out + int_to_text(xs[i]);\n"
            "    }\n    ret out;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

# ---- template: number theory (is-prime, count-primes<=N, sum-of-divisors, perfect-number) ----
# NOTE: avoids gcd and sieve (benchmark tasks); uses trial-division primality instead.
def number_theory_drills(seed):
    rng = random.Random(seed)
    p = rng.randint(2, 200)
    limit = rng.randint(10, 60)
    d = rng.randint(6, 100)
    perf_candidate = rng.choice([6, 28, 12, 18, 24, 496, 8, 10])
    # oracle helpers
    def is_prime(x):
        if x < 2:
            return False
        i = 2
        while i * i <= x:
            if x % i == 0:
                return False
            i += 1
        return True
    def sum_div(x):
        s = 0
        i = 1
        while i <= x:
            if x % i == 0:
                s += i
            i += 1
        return s
    isp = is_prime(p)
    cnt = sum(1 for k in range(2, limit + 1) if is_prime(k))
    sd = sum_div(d)
    # perfect: sum of proper divisors (exclude itself) == itself
    proper = sum_div(perf_candidate) - perf_candidate
    perfect = (proper == perf_candidate)
    out = [f"is_prime({p}) = {'true' if isp else 'false'}",
           f"primes_up_to({limit}) = {cnt}",
           f"sum_of_divisors({d}) = {sd}",
           f"is_perfect({perf_candidate}) = {'true' if perfect else 'false'}"]
    body = [f"    let p: Int = {p};",
            f"    let limit: Int = {limit};",
            f"    let d: Int = {d};",
            f"    let pc: Int = {perf_candidate};",
            "    let isp: Bool = isPrime(p);",
            "    let cnt: Int = countPrimes(limit);",
            "    let sd: Int = sumOfDivisors(d);",
            "    let perfect: Bool = isPerfect(pc);",
            "    fx {",
            '        println("is_prime(", p, ") = ", isp);',
            '        println("primes_up_to(", limit, ") = ", cnt);',
            '        println("sum_of_divisors(", d, ") = ", sd);',
            '        println("is_perfect(", pc, ") = ", perfect);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: number theory by trial division — primality test (i*i <= n loop), "
           "counting primes up to N, sum of all divisors, perfect-number check via proper-divisor sum; "
           "Bool output\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn isPrime(n: Int) -> Bool {\n"
            "    if n < 2 { ret false; }\n"
            "    let i: Int = 2;\n"
            "    loop i * i <= n {\n"
            "        if n % i == 0 { ret false; }\n"
            "        i = i + 1;\n"
            "    }\n    ret true;\n}\n\n"
            "fn countPrimes(limit: Int) -> Int {\n"
            "    let count: Int = 0;\n"
            "    loop k in 2..limit + 1 {\n"
            "        if isPrime(k) { count = count + 1; }\n"
            "    }\n    ret count;\n}\n\n"
            "fn sumOfDivisors(n: Int) -> Int {\n"
            "    let total: Int = 0;\n"
            "    let i: Int = 1;\n"
            "    loop i <= n {\n"
            "        if n % i == 0 { total = total + i; }\n"
            "        i = i + 1;\n"
            "    }\n    ret total;\n}\n\n"
            "fn isPerfect(n: Int) -> Bool {\n"
            "    let proper: Int = sumOfDivisors(n) - n;\n"
            "    if proper == n { ret true; }\n    ret false;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

# ---- template: strings (reverse, palindrome check, vowel count, char frequency) ----
# Aether Text is 1-based: valid indices are 1..string_len(s).
def string_drills(seed):
    rng = random.Random(seed)
    WORDS = ["education", "racecar", "balloon", "mississippi", "programming",
             "level", "banana", "rotator", "alphabet", "committee", "deed",
             "kayak", "noon", "pepper", "civic", "tattarrattat", "radar",
             "stats", "redder", "minim", "refer", "sequoia", "rhythm"]
    word = rng.choice(WORDS)
    # frequency target: a letter that appears in the word (deterministic) plus
    # occasionally one that doesn't, to exercise the zero case
    if rng.random() < 0.75:
        target = rng.choice(list(word))
    else:
        target = rng.choice([c for c in "qwxyz" if c not in word] or ["z"])
    # oracle
    rev = word[::-1]
    is_pal = (word == rev)
    vowels = sum(1 for c in word if c in "aeiou")
    freq = word.count(target)
    out = [f"word: {word}",
           f"reversed: {rev}",
           f"is_palindrome = {'true' if is_pal else 'false'}",
           f"vowel_count = {vowels}",
           f"freq('{target}') = {freq}"]
    body = [f'    let word: Text = "{word}";',
            f'    let target: Text = "{target}";',
            "    let rev: Text = reverseStr(word);",
            "    let pal: Bool = isPalindrome(word);",
            "    let vowels: Int = vowelCount(word);",
            "    let freq: Int = charFreq(word, target);",
            "    fx {",
            '        println("word: ", word);',
            '        println("reversed: ", rev);',
            '        println("is_palindrome = ", pal);',
            '        println("vowel_count = ", vowels);',
            '        println("freq(\'", target, "\') = ", freq);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: Text processing with 1-based character indexing (s[1]..s[string_len(s)]) — "
           "reverse by prepend-scan, palindrome check via string equality, vowel counting by "
           "per-char test, single-character frequency tally\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn reverseStr(s: Text) -> Text {\n"
            "    let out: Text = \"\";\n"
            "    let n: Int = string_len(s);\n"
            "    let i: Int = n;\n"
            "    loop i >= 1 {\n"
            "        out = out + s[i];\n"
            "        i = i - 1;\n"
            "    }\n    ret out;\n}\n\n"
            "fn isPalindrome(s: Text) -> Bool {\n"
            "    let n: Int = string_len(s);\n"
            "    let i: Int = 1;\n"
            "    let j: Int = n;\n"
            "    loop i < j {\n"
            "        if s[i] != s[j] { ret false; }\n"
            "        i = i + 1;\n"
            "        j = j - 1;\n"
            "    }\n    ret true;\n}\n\n"
            "fn isVowel(c: Text) -> Bool {\n"
            "    if c == \"a\" { ret true; }\n"
            "    if c == \"e\" { ret true; }\n"
            "    if c == \"i\" { ret true; }\n"
            "    if c == \"o\" { ret true; }\n"
            "    if c == \"u\" { ret true; }\n"
            "    ret false;\n}\n\n"
            "fn vowelCount(s: Text) -> Int {\n"
            "    let count: Int = 0;\n"
            "    let n: Int = string_len(s);\n"
            "    loop i in 1..n + 1 {\n"
            "        if isVowel(s[i]) { count = count + 1; }\n"
            "    }\n    ret count;\n}\n\n"
            "fn charFreq(s: Text, target: Text) -> Int {\n"
            "    let count: Int = 0;\n"
            "    let n: Int = string_len(s);\n"
            "    loop i in 1..n + 1 {\n"
            "        if s[i] == target { count = count + 1; }\n"
            "    }\n    ret count;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

# ---- template: graphs (reachability via DFS + connected-component count; flat adjacency matrix) ----
# NOTE: avoids bfs/dijkstra (benchmark tasks); uses recursive DFS over a flat n*n
# undirected adjacency matrix indexed u*n+v.
def graph_drills(seed):
    rng = random.Random(seed)
    n = rng.randint(4, 7)
    # build a random undirected simple graph (no self loops)
    adj = [0] * (n * n)
    for u in range(n):
        for v in range(u + 1, n):
            if rng.random() < 0.4:
                adj[u * n + v] = 1
                adj[v * n + u] = 1
    src = rng.randrange(n)
    # oracle: DFS reachable count from src
    def reach_count(start):
        seen = [False] * n
        stack = [start]
        seen[start] = True
        c = 0
        while stack:
            u = stack.pop()
            c += 1
            for v in range(n):
                if adj[u * n + v] == 1 and not seen[v]:
                    seen[v] = True
                    stack.append(v)
        return c
    reach = reach_count(src)
    # oracle: number of connected components
    comp_seen = [False] * n
    comps = 0
    for s in range(n):
        if not comp_seen[s]:
            comps += 1
            stack = [s]
            comp_seen[s] = True
            while stack:
                u = stack.pop()
                for v in range(n):
                    if adj[u * n + v] == 1 and not comp_seen[v]:
                        comp_seen[v] = True
                        stack.append(v)
    # edge count (undirected)
    edges = sum(adj) // 2
    out = [f"nodes: {n}",
           f"edges: {edges}",
           f"reachable_from({src}) = {reach}",
           f"components = {comps}"]
    body = [f"    let adj: Int[] = [{', '.join(str(v) for v in adj)}];",
            f"    let n: Int = {n};",
            f"    let src: Int = {src};",
            "    let edges: Int = countEdges(adj, n);",
            "    let reach: Int = reachableFrom(adj, n, src);",
            "    let comps: Int = componentCount(adj, n);",
            "    fx {",
            '        println("nodes: ", n);',
            '        println("edges: ", edges);',
            '        println("reachable_from(", src, ") = ", reach);',
            '        println("components = ", comps);',
            "    }"]
    hdr = ("// {name}\n"
           "// Concepts: undirected graph as a flat n*n adjacency matrix (adj[u*n+v]) — "
           "recursive DFS reachable-node count from a source, connected-component count by "
           "scanning unvisited roots, undirected edge count; visited[] passed by reference\n"
           + "".join(f"// expect: {l}\n" for l in out))
    prog = ("\nfn dfs(adj: Int[], visited: Int[], n: Int, u: Int) -> Int {\n"
            "    visited[u] = 1;\n"
            "    let count: Int = 1;\n"
            "    loop v in 0..n {\n"
            "        if adj[u * n + v] == 1 {\n"
            "            if visited[v] == 0 { count = count + dfs(adj, visited, n, v); }\n"
            "        }\n"
            "    }\n    ret count;\n}\n\n"
            "fn reachableFrom(adj: Int[], n: Int, src: Int) -> Int {\n"
            "    let visited: Int[] = [];\n"
            "    loop i in 0..n { visited = visited + [0]; }\n"
            "    ret dfs(adj, visited, n, src);\n}\n\n"
            "fn componentCount(adj: Int[], n: Int) -> Int {\n"
            "    let visited: Int[] = [];\n"
            "    loop i in 0..n { visited = visited + [0]; }\n"
            "    let comps: Int = 0;\n"
            "    loop s in 0..n {\n"
            "        if visited[s] == 0 {\n"
            "            comps = comps + 1;\n"
            "            let ignore: Int = dfs(adj, visited, n, s);\n"
            "        }\n"
            "    }\n    ret comps;\n}\n\n"
            "fn countEdges(adj: Int[], n: Int) -> Int {\n"
            "    let total: Int = 0;\n"
            "    loop u in 0..n {\n"
            "        loop v in 0..n {\n"
            "            if adj[u * n + v] == 1 { total = total + 1; }\n"
            "        }\n"
            "    }\n    ret total / 2;\n}\n\n"
            "fn main() -> Void {\n" + "\n".join(body) + "\n    ret;\n}\n")
    return hdr, prog, "\n".join(out)

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
        ("306_recursion", recursion_drills, 32, 7000),
        ("307_sorting", sorting_drills, 32, 8000),
        ("308_searching", searching_drills, 32, 9000),
        ("309_dp1d", dp1d_drills, 32, 10000),
        ("310_dp2d", dp2d_drills, 32, 11000),
        ("311_number_theory", number_theory_drills, 32, 12000),
        ("312_strings", string_drills, 32, 13000),
        ("313_graphs", graph_drills, 32, 14000),
    ]
    total = 0
    for prefix, fn, target, seed0 in plan:
        if only and only not in prefix:
            continue
        total += run_template(prefix, fn, target, seed0)
    print(f"TOTAL new kept: {total}")
