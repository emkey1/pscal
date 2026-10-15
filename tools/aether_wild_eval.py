#!/usr/bin/env python3
"""Wild-eval prototype (see Docs/aether_benchmark_gaps.md §8).

Generates novel, single-skill Aether tasks from parameterized templates, each
paired with a *trusted Python oracle* that computes the expected stdout. Two
modes:

  validate  (default, CPU-only) -- for every generated task, compile+run a
            reference Aether solution and confirm its output equals the Python
            oracle. This proves the generator and oracle are mutually
            consistent (the task is Aether-feasible and unambiguous).

  score     (--endpoint URL --model NAME) -- query a served model with each
            task prompt (no-guide style), sanitize + compile its Aether, and
            compare to the oracle. Reports a "wild" generalization rate on
            never-before-seen tasks.

  negatives (CPU-only) -- break each generated reference in the ways a model
            gets Aether wrong (the fx block removed, an effect inside @pure,
            `return` for `ret`, a builtin with the wrong argument count, a
            misspelled type, a contract that cannot hold), run every broken
            program and keep only those that fail with exactly the one error
            code the break is meant to cause. With --dump the kept programs
            are written as `should_fail` items in the bench manifest format,
            which `tools/aether_oracle_check.py --tasks <out>` checks.

--paraphrases N (validate and score) gives every task N wordings of the same
request. Validate also checks that each wording carries every value and the
output format the one reference answer depends on; score asks the model each
wording at temperature 0 and reports, per template, how many tasks kept one
pass/fail verdict across all N wordings (a change comes from the wording
alone). N=1 is the original single prompt, and its output is unchanged.

--suite properties swaps the expected-output tasks for property tasks: the
model writes one function with a fixed signature, and the harness puts @post
properties on it and calls it on generated inputs, so no expected answer is
needed. A property that fails stops the program with CON-001 and the attempt
sorts as contract_violation (aether_doc_bench.classify_attempt). Validate runs
each task's reference (must pass) and its deliberately broken versions (each
must be caught as a contract violation).

The generator is deterministic given --seed, so a run is reproducible. Templates
draw from the §1 mechanism inventory; this is the smoke-scale set, easily
extended. Tasks are *not* frozen here -- freezing a curated subset is what turns
this into a benchmark (gaps doc §8, "same generator, two products").
"""
from __future__ import annotations
import argparse
import json
import os
import random
import re
import subprocess
import tempfile
import urllib.request

AETHER_BIN = os.path.abspath(os.environ.get("AETHER_BIN", "build/bin/aether"))
END_MARKER = "__AETHER_BENCH_END__"


# ---- byte-level-BPE artifact decode (mirrors aether_doc_bench.sanitize_code) ----
def _bytelevel_map() -> dict[str, int]:
    bs = (list(range(ord("!"), ord("~") + 1))
          + list(range(ord("¡"), ord("¬") + 1))
          + list(range(ord("®"), ord("ÿ") + 1)))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return {chr(c): b for b, c in zip(bs, cs)}


_BL = _bytelevel_map()


def sanitize(raw: str) -> str:
    if "Ġ" in raw or "Ċ" in raw:
        if raw and all(ch in _BL for ch in raw):
            try:
                raw = bytes(_BL[ch] for ch in raw).decode("utf-8", "replace")
            except Exception:
                pass
    i = raw.find("</think>")
    if i != -1:
        raw = raw[i + len("</think>"):]
    j = raw.find(END_MARKER)
    if j != -1:
        raw = raw[:j]
    lines = raw.strip().splitlines()
    if lines and lines[0].startswith("```"):
        lines = lines[1:]
    while lines and lines[-1].strip() == "```":
        lines.pop()
    return "\n".join(lines).strip()


# ---- templates: each returns dict(template, prompt, expected_stdout, reference, mechanisms) ----
def _nest(fn: str, terms: list[str]) -> str:
    acc = terms[0]
    for t in terms[1:]:
        acc = f"{fn}({acc}, {t})"
    return acc


def t_mean(rng: random.Random) -> dict:
    k = rng.choice([3, 4, 5])
    vals = [rng.randint(0, 100) for _ in range(k)]
    expected = f"mean = {sum(vals) / k:.2f}\n"
    ref = (f"fn main() -> Void {{\n    let total: Int = {' + '.join(map(str, vals))};\n"
           f"    let avg: Real = total * 1.0 / {k};\n"
           f"    fx {{ println(\"mean = \", avg:0:2); }}\n    ret;\n}}\n")
    lst = ", ".join(map(str, vals))
    wordings = [
        f"Print the arithmetic mean of these {k} integers, rounded to exactly two "
        f"decimal places, as `mean = <value>`: {lst}.",
        f"Given the integers {lst}, compute their average and print it with exactly two digits "
        f"after the decimal point, in the form `mean = <value>`.",
        f"Output `mean = <value>`, where <value> is the mean of {lst} shown to two decimal places.",
        f"There are {k} numbers: {lst}. Print their arithmetic mean to 2 decimal places as `mean = <value>`.",
    ]
    return dict(template="mean", prompt=wordings[0], expected_stdout=expected,
                reference=ref, mechanisms=["real", "arithmetic"],
                wordings=wordings, facts=[*map(str, vals), "mean = <value>"])


def t_sum(rng: random.Random) -> dict:
    k = rng.choice([3, 4, 5, 6])
    vals = [rng.randint(1, 50) for _ in range(k)]
    expected = f"sum = {sum(vals)}\n"
    ref = (f"fn main() -> Void {{\n    let s: Int = {' + '.join(map(str, vals))};\n"
           f"    fx {{ println(\"sum = \", s); }}\n    ret;\n}}\n")
    lst = ", ".join(map(str, vals))
    wordings = [
        f"Print the sum of these integers as `sum = <value>`: {lst}.",
        f"Add up {lst} and print the total as `sum = <value>`.",
        f"Print `sum = <value>`, where <value> is the total of these integers: {lst}.",
        f"Compute the sum of {lst}. The output line must be `sum = <value>`.",
    ]
    return dict(template="sum", prompt=wordings[0], expected_stdout=expected,
                reference=ref, mechanisms=["arithmetic"],
                wordings=wordings, facts=[*map(str, vals), "sum = <value>"])


def t_clamp(rng: random.Random) -> dict:
    lo = rng.randint(0, 20)
    hi = lo + rng.randint(40, 80)
    val = rng.randint(lo - 30, hi + 30)
    expected = f"{max(lo, min(hi, val))}\n"
    ref = f"fn main() -> Void {{\n    fx {{ println(clamp({val}, {lo}, {hi})); }}\n    ret;\n}}\n"
    wordings = [
        f"Print {val} clamped to the inclusive range {lo} to {hi}.",
        f"Print the value of {val} restricted to the closed interval [{lo}, {hi}].",
        f"Clamp {val} so it is no less than {lo} and no more than {hi}, then print it.",
        f"With a lower bound of {lo} and an upper bound of {hi} (both inclusive), print {val} "
        f"clamped to that range.",
    ]
    return dict(template="clamp", prompt=wordings[0], expected_stdout=expected,
                reference=ref, mechanisms=["clamp", "builtin"],
                wordings=wordings, facts=[str(val), str(lo), str(hi)])


def t_max(rng: random.Random) -> dict:
    k = rng.choice([3, 4, 5])
    vals = [rng.randint(0, 200) for _ in range(k)]
    expected = f"max = {max(vals)}\n"
    nested = _nest("mx", [str(v) for v in vals])
    ref = ("@pure\nfn mx(a: Int, b: Int) -> Int {\n    if a > b { ret a; }\n    ret b;\n}\n"
           f"fn main() -> Void {{\n    let m: Int = {nested};\n"
           f"    fx {{ println(\"max = \", m); }}\n    ret;\n}}\n")
    lst = ", ".join(map(str, vals))
    wordings = [
        f"Print the largest of these integers as `max = <value>`: {lst}.",
        f"Find the maximum of {lst} and print it as `max = <value>`.",
        f"Print `max = <value>`, where <value> is the biggest number among {lst}.",
        f"Which of these integers is largest: {lst}? Print it as `max = <value>`.",
    ]
    return dict(template="max", prompt=wordings[0], expected_stdout=expected,
                reference=ref, mechanisms=["branching", "pure"],
                wordings=wordings, facts=[*map(str, vals), "max = <value>"])


def t_count_above(rng: random.Random) -> dict:
    k = rng.choice([4, 5, 6])
    vals = [rng.randint(0, 100) for _ in range(k)]
    thr = rng.randint(30, 70)
    expected = f"{sum(1 for v in vals if v >= thr)}\n"
    terms = " + ".join(f"ge({v}, {thr})" for v in vals)
    ref = ("@pure\nfn ge(v: Int, t: Int) -> Int {\n    if v >= t { ret 1; }\n    ret 0;\n}\n"
           f"fn main() -> Void {{\n    let c: Int = {terms};\n"
           f"    fx {{ println(c); }}\n    ret;\n}}\n")
    lst = ", ".join(map(str, vals))
    wordings = [
        f"Count how many of these integers are greater than or equal to {thr}, and print "
        f"just that count: {lst}.",
        f"Print only the number of values in {lst} that are at least {thr}.",
        f"How many of the integers {lst} are {thr} or more? Print just the count.",
        f"Count the integers in this list that are not below {thr} and print only that number: {lst}.",
    ]
    return dict(template="count_above", prompt=wordings[0], expected_stdout=expected,
                reference=ref, mechanisms=["pure", "branching"],
                wordings=wordings, facts=[*map(str, vals), str(thr)])


TEMPLATES = [t_mean, t_sum, t_clamp, t_max, t_count_above]
# Every template has this many wordings; --paraphrases may ask for up to it.
MAX_PARAPHRASES = 4


def generate(n: int, seed: int, paraphrases: int = 1) -> list[dict]:
    """Tasks for --seed. Wordings draw nothing from the rng, so the tasks (and,
    with paraphrases=1, the dicts) are the same for every N; with N > 1 each
    task also carries `paraphrases` (wording 0 is `prompt`) and the `facts`
    every wording must state."""
    rng = random.Random(seed)
    out = []
    for i in range(n):
        tmpl = TEMPLATES[i % len(TEMPLATES)]
        task = tmpl(rng)
        wordings, facts = task.pop("wordings"), task.pop("facts")
        if paraphrases > 1:
            task["paraphrases"] = wordings[:paraphrases]
            task["facts"] = facts
        task["id"] = f"{task['template']}_{i:03d}"
        out.append(task)
    return out


def missing_facts(wording: str, facts: list[str]) -> list[str]:
    """Facts a wording leaves out: a number must appear as a whole number (52
    is not found in 152), a format string verbatim."""
    return [f for f in facts
            if not (re.search(rf"(?<![\d-]){re.escape(f)}(?!\d)", wording) if f.lstrip("-").isdigit()
                    else f in wording)]


def consistency_report(verdicts: dict[str, list[int]], templates: dict[str, str]) -> list[str]:
    """Lines saying, per template, how many tasks kept one verdict for every
    wording, and per task how often the verdict changed between wordings."""
    lines, by_t = [], {}
    for task_id, v in verdicts.items():
        flips = sum(a != b for a, b in zip(v, v[1:]))
        by_t.setdefault(templates[task_id], []).append(len(set(v)) == 1)
        lines.append(f"  {task_id:<16} {''.join('P' if x else '.' for x in v)}  passes {sum(v)}/{len(v)}, "
                     f"verdict changes {flips}")
    stable = sum(sum(x) for x in by_t.values())
    lines.append(f"\nconsistency: {stable}/{len(verdicts)} tasks kept one verdict for every wording")
    for k, x in sorted(by_t.items()):
        lines.append(f"  {k:<14} {sum(x)}/{len(x)} consistent")
    return lines


# ---- should-fail generator: break a reference the way a model breaks Aether ----
# Each break takes a template's reference and the run's rng and returns
# (program, note), or None when the reference has nothing that break can touch.
# The code is the one error the broken program must give and nothing else.
_TYPO = {"Int": ["Itn", "Integr", "Imt"], "Real": ["Rael", "Reel", "Raal"]}
_PURE_FN = re.compile(r"@pure\nfn (\w+)\((\w+): \w+")
_FIRST_LET = re.compile(r"(let \w+: Int = )(.+);")
_CHECKER = "@pure\nfn chk(v: Int) -> Int {\n    ret v;\n}\n"


def b_no_fx(ref: str, rng: random.Random):
    if "fx { " not in ref:
        return None
    return re.sub(r"fx \{ (.*?) \}", r"\1", ref, count=1), "the fx block around println removed"


def b_effect_in_pure(ref: str, rng: random.Random):
    m = _PURE_FN.search(ref)
    if m:
        line_end = ref.index("\n", m.end()) + 1
        return (ref[:line_end] + f"    fx {{ println({m.group(2)}); }}\n" + ref[line_end:],
                f"println inside @pure fn {m.group(1)}")
    return ref.replace("fn main()", "@pure\nfn main()", 1), "main marked @pure around its fx block"


def b_return_for_ret(ref: str, rng: random.Random):
    spots = [m.start() for m in re.finditer(r"\bret\b", ref)]
    if not spots:
        return None
    at = rng.choice(spots)
    return ref[:at] + "return" + ref[at + 3:], "`return` written for `ret`"


def b_builtin_arity(ref: str, rng: random.Random):
    m = re.search(r"clamp\(([^()]+)\)", ref)
    if m:
        args = [a.strip() for a in m.group(1).split(",")]
        bad = args[:-1] if rng.random() < 0.5 else args + [args[-1]]
        return (ref[:m.start(1)] + ", ".join(bad) + ref[m.end(1):],
                f"clamp called with {len(bad)} arguments (takes 3)")
    m = _FIRST_LET.search(ref)
    if not m:
        return None
    fn, bad = rng.choice([("max", m.group(2)), ("min", f"{m.group(2)}, 0, 0")])
    return (ref[:m.start(2)] + f"{fn}({bad})" + ref[m.end(2):],
            f"{fn} called with {1 if fn == 'max' else 3} arguments (takes 2)")


def b_misspelled_type(ref: str, rng: random.Random):
    # Not Void: a misspelled `-> Void` reports FLOW-002 (the `ret;` lacks a
    # value) before the unknown type, so it would never survive the filter.
    spots = list(re.finditer(r"(?<=: |> )(Int|Real)\b", ref))
    if not spots:
        return None
    m = rng.choice(spots)
    typo = rng.choice(_TYPO[m.group(1)])
    return ref[:m.start()] + typo + ref[m.end():], f"type {m.group(1)} misspelled {typo}"


def b_failing_contract(ref: str, rng: random.Random):
    side = rng.choice(["pre", "post"])
    m = _PURE_FN.search(ref)
    if m:
        cond = f"{m.group(2)} < {m.group(2)}" if side == "pre" else "result < result"
        at = m.start()
        return ref[:at] + f"@{side}({cond})\n" + ref[at:], f"@{side}({cond}) on {m.group(1)} can never hold"
    # No function to annotate: route an Int value through a checker (an Int
    # `let`, else the clamp call; mean's printed value is a formatted Real).
    cond = "v < v" if side == "pre" else "result < result"
    m = _FIRST_LET.search(ref) or re.search(r"(println\()(clamp\([^()]*\))\)", ref)
    if not m:
        return None
    prog = ref[:m.start(2)] + f"chk({m.group(2)})" + ref[m.end(2):]
    return f"@{side}({cond})\n" + _CHECKER + prog, f"@{side}({cond}) on a checker the value passes through"


BREAKS = [("no_fx", "FX-001", b_no_fx),
          ("effect_in_pure", "ANN-001", b_effect_in_pure),
          ("return_for_ret", "SYN-001", b_return_for_ret),
          ("builtin_arity", "BUILT-002", b_builtin_arity),
          ("misspelled_type", "TYPE-002", b_misspelled_type),
          ("failing_contract", "CON-001", b_failing_contract)]
_CODE = re.compile(r"\[([A-Z]+-\d{3})\]")


def break_candidates(tasks: list[dict], seed: int) -> list[dict]:
    """Every break of every task, as should_fail items (not yet run). The
    breaks draw from their own rng, so --seed fixes them as it fixes the tasks."""
    rng = random.Random(f"negatives:{seed}")
    out = []
    for t in tasks:
        for name, code, fn in BREAKS:
            got = fn(t["reference"], rng)
            if got is None:
                continue
            program, note = got
            out.append({"id": f"neg_{t['id']}_{name}", "title": f"{t['template']}: {note}",
                        "should_fail": True, "program": program, "expected_error_code": code,
                        "mechanisms": ["negative", name] + t["mechanisms"],
                        "note": f"Generated by aether_wild_eval.py --mode negatives --seed {seed} "
                                f"from {t['id']}: {note}.",
                        "source_task": t["id"], "break": name})
    return out


def exact_failure(rc: int, text: str, code: str) -> bool:
    """The broken program failed, and the only code it printed is the expected one."""
    return rc != 0 and set(_CODE.findall(text)) == {code}


def generate_negatives(tasks: list[dict], seed: int, run=None) -> tuple[list[dict], list[dict]]:
    """(kept, dropped): run each candidate, keep those that fail exactly as meant."""
    run = run or (lambda src: run_aether(src, with_stderr=True))
    kept, dropped = [], []
    for item in break_candidates(tasks, seed):
        rc, text = run(item["program"])
        if exact_failure(rc, text, item["expected_error_code"]):
            kept.append(item)
        else:
            dropped.append({**item, "observed_rc": rc, "observed_codes": sorted(set(_CODE.findall(text))),
                            "observed_first_line": re.sub(r"^\S*prog\.aether:", "line ",
                                                          (text.strip().splitlines() or [""])[0])[:200]})
    return kept, dropped


# ---- property tasks: a fixed signature, @post properties, generated inputs ----
# The model writes only the function; wrap_property() puts the properties on
# it and appends a main that calls it on every input. No expected answer: the
# run passes when no property fails and main prints its `checked N` line.
def _prop(name, sig, spec, posts, inputs, reference, broken, mechanisms):
    calls = [f"{name}({', '.join(map(str, args))})" for args in inputs]
    return dict(template=name, kind="property", function=name, signature=sig,
                prompt=(f"Write only this Aether function, with exactly this signature and no main: "
                        f"`{sig}`. {spec}"),
                posts=posts, calls=calls, expected_stdout=f"checked {len(calls)}\n",
                reference=reference, broken=broken, mechanisms=mechanisms)


def p_clamp_to(rng: random.Random) -> dict:
    lo = rng.randint(-20, 20)
    hi = lo + rng.randint(5, 40)
    vs = [lo - rng.randint(1, 30), lo, rng.randint(lo + 1, hi - 1), hi, hi + rng.randint(1, 30),
          rng.randint(lo - 50, hi + 50)]
    sig = "fn clamp_to(v: Int, lo: Int, hi: Int) -> Int"
    return _prop("clamp_to", sig, "It returns v limited to the inclusive range lo to hi (lo <= hi).",
                 ["result >= lo && result <= hi", "v >= lo || result == lo", "v <= hi || result == hi",
                  "v < lo || v > hi || result == v"],
                 [(v, lo, hi) for v in vs],
                 f"@pure\n{sig} {{\n    if v < lo {{ ret lo; }}\n    if v > hi {{ ret hi; }}\n    ret v;\n}}\n",
                 {"returns_v": f"{sig} {{\n    ret v;\n}}\n",
                  "no_upper_bound": f"{sig} {{\n    if v < lo {{ ret lo; }}\n    ret v;\n}}\n",
                  "bounds_swapped": f"{sig} {{\n    if v < lo {{ ret hi; }}\n    if v > hi {{ ret lo; }}\n    ret v;\n}}\n"},
                 ["contract", "branching"])


def p_abs_val(rng: random.Random) -> dict:
    xs = [-rng.randint(1, 500), 0, rng.randint(1, 500), -rng.randint(1, 9), rng.randint(-1000, 1000)]
    sig = "fn abs_val(x: Int) -> Int"
    return _prop("abs_val", sig, "It returns the absolute value of x, without calling abs.",
                 ["result >= 0", "result == x || result == 0 - x"],
                 [(x,) for x in xs],
                 f"@pure\n{sig} {{\n    if x < 0 {{ ret 0 - x; }}\n    ret x;\n}}\n",
                 {"returns_x": f"{sig} {{\n    ret x;\n}}\n",
                  "always_negates": f"{sig} {{\n    ret 0 - x;\n}}\n"},
                 ["contract", "branching"])


def p_max3(rng: random.Random) -> dict:
    a, b, c = sorted(rng.sample(range(-100, 100), 3))
    triples = [(c, a, b), (a, c, b), (a, b, c), (b, b, a), (rng.randint(-50, 50),) * 3]
    sig = "fn max3(a: Int, b: Int, c: Int) -> Int"
    return _prop("max3", sig, "It returns the largest of a, b and c.",
                 ["result >= a && result >= b && result >= c", "result == a || result == b || result == c"],
                 triples,
                 f"@pure\n{sig} {{\n    let m: Int = a;\n    if b > m {{ m = b; }}\n    if c > m {{ m = c; }}\n    ret m;\n}}\n",
                 {"ignores_c": f"{sig} {{\n    if a > b {{ ret a; }}\n    ret b;\n}}\n",
                  "returns_min": f"{sig} {{\n    let m: Int = a;\n    if b < m {{ m = b; }}\n    if c < m {{ m = c; }}\n    ret m;\n}}\n"},
                 ["contract", "branching"])


def p_sign_of(rng: random.Random) -> dict:
    xs = [-rng.randint(1, 900), 0, rng.randint(1, 900), -1, 1]
    rng.shuffle(xs)
    sig = "fn sign_of(x: Int) -> Int"
    return _prop("sign_of", sig, "It returns 1 when x is positive, -1 when x is negative and 0 when x is 0.",
                 ["x <= 0 || result == 1", "x >= 0 || result == 0 - 1", "x != 0 || result == 0"],
                 [(x,) for x in xs],
                 f"@pure\n{sig} {{\n    if x > 0 {{ ret 1; }}\n    if x < 0 {{ ret 0 - 1; }}\n    ret 0;\n}}\n",
                 {"zero_is_positive": f"{sig} {{\n    if x < 0 {{ ret 0 - 1; }}\n    ret 1;\n}}\n",
                  "sign_flipped": f"{sig} {{\n    if x > 0 {{ ret 0 - 1; }}\n    if x < 0 {{ ret 1; }}\n    ret 0;\n}}\n"},
                 ["contract", "branching"])


def p_isqrt(rng: random.Random) -> dict:
    k = rng.randint(3, 40)
    ns = [0, 1, k * k, k * k - 1, rng.randint(2, 5000)]
    sig = "fn isqrt(n: Int) -> Int"
    return _prop("isqrt", sig, "For n >= 0 it returns the integer square root: the largest r with r * r <= n.",
                 ["result >= 0", "result * result <= n", "(result + 1) * (result + 1) > n"],
                 [(n,) for n in ns],
                 f"@pure\n{sig} {{\n    let r: Int = 0;\n    while (r + 1) * (r + 1) <= n {{\n        r = r + 1;\n    }}\n    ret r;\n}}\n",
                 {"half": f"{sig} {{\n    ret n / 2;\n}}\n",
                  "off_by_one": f"{sig} {{\n    let r: Int = 0;\n    while r * r <= n {{\n        r = r + 1;\n    }}\n    ret r;\n}}\n"},
                 ["contract", "loops"])


PROPERTY_TEMPLATES = [p_clamp_to, p_abs_val, p_max3, p_sign_of, p_isqrt]


def generate_properties(n: int, seed: int) -> list[dict]:
    rng = random.Random(f"properties:{seed}")
    out = []
    for i in range(n):
        task = PROPERTY_TEMPLATES[i % len(PROPERTY_TEMPLATES)](rng)
        task["id"] = f"prop_{task['template']}_{i:03d}"
        out.append(task)
    return out


def _drop_main(code: str) -> str:
    """The function alone: a `fn main` the model added anyway is cut out
    (brace-matched), since the harness supplies its own."""
    at = code.find("fn main(")
    if at < 0:
        return code
    open_at = code.find("{", at)
    if open_at < 0:
        return code[:at]
    depth = 0
    for i in range(open_at, len(code)):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        if depth == 0:
            return code[:at] + code[i + 1:]
    return code[:at]


def wrap_property(task: dict, fn_source: str) -> str:
    """The checked program: the properties as @post lines above the function
    (above any annotations it already has; @post must come first) and a main
    that calls it on every input."""
    lines = _drop_main(fn_source).rstrip().splitlines()
    head = next((i for i, ln in enumerate(lines)
                 if re.match(rf"\s*fn {re.escape(task['function'])}\s*\(", ln)), None)
    if head is None:
        lines.insert(0, "")  # no such function: compiles to an error naming it
        head = 0
    while head > 0 and lines[head - 1].lstrip().startswith("@"):
        head -= 1
    posts = [f"@post({p})" for p in task["posts"]]
    lets = "".join(f"    let r{i}: Int = {c};\n" for i, c in enumerate(task["calls"]))
    main = f"fn main() -> Void {{\n{lets}    fx {{ println(\"checked \", {len(task['calls'])}); }}\n    ret;\n}}\n"
    return "\n".join(lines[:head] + posts + lines[head:]) + "\n" + main


def run_property(task: dict, fn_source: str, run=None) -> str:
    """The attempt's class (pass, contract_violation, coded_error, ...), sorted
    by the bench's own classify_attempt."""
    import aether_doc_bench as adb  # the bench's classifier; heavy, so only here
    run = run or run_aether_full
    rc, out, err = run(wrap_property(task, fn_source))
    return adb.classify_attempt({"generated_ok": True, "source_code": fn_source, "run": {
        "returncode": rc, "stdout": out, "stderr": err, "expected_returncode": 0,
        "exact_stdout_match": rc == 0 and out == task["expected_stdout"]}})


def run_aether_full(source: str) -> tuple[int, str, str]:
    with tempfile.TemporaryDirectory(prefix="wild-eval-") as td:
        p = os.path.join(td, "prog.aether")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(source)
        proc = subprocess.run([AETHER_BIN, "--no-cache", p], cwd=td,
                              capture_output=True, text=True, errors="replace", timeout=30)
        return proc.returncode, proc.stdout, proc.stderr


def run_aether(source: str, with_stderr: bool = False) -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="wild-eval-") as td:
        p = os.path.join(td, "prog.aether")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(source)
        proc = subprocess.run([AETHER_BIN, "--no-cache", p], cwd=td,
                              capture_output=True, text=True, errors="replace", timeout=30)
        return proc.returncode, proc.stdout + (proc.stderr if with_stderr else "")


def query_model(prompt: str, endpoint: str, model: str, what: str = "one complete Aether program") -> str:
    full = (f"You are writing Aether code. Write exactly {what} for the "
            f"task below. Return only raw Aether source code, no Markdown fences, no explanation. "
            f"After the program, output a final line containing exactly {END_MARKER}.\n\nTask:\n{prompt}")
    body = json.dumps({"model": model, "messages": [{"role": "user", "content": full}],
                       "max_tokens": 500, "temperature": 0}).encode()
    req = urllib.request.Request(endpoint, data=body, headers={"Content-Type": "application/json"})
    resp = json.loads(urllib.request.urlopen(req, timeout=120).read().decode("utf-8", "replace"))
    return resp["choices"][0]["message"]["content"]


def properties_main(args, ap) -> int:
    if args.mode == "negatives" or args.paraphrases != 1:
        ap.error("--suite properties takes --mode validate or score, one wording")
    tasks = generate_properties(args.n, args.seed)
    if args.dump:
        with open(args.dump, "w", encoding="utf-8") as fh:
            json.dump({"seed": args.seed, "suite": "properties", "tasks": tasks}, fh, indent=2)
    if args.mode == "validate":
        bad = caught = total = 0
        for t in tasks:
            ref = run_property(t, t["reference"])
            bad += ref != "pass"
            broken = {name: run_property(t, src) for name, src in t["broken"].items()}
            total += len(broken)
            caught += sum(cls == "contract_violation" for cls in broken.values())
            line = [f"reference {ref}"] + [f"{name} {cls}" for name, cls in broken.items()]
            ok = ref == "pass" and all(cls == "contract_violation" for cls in broken.values())
            print(f"[{'ok ' if ok else 'BAD'}] {t['id']:<22} " + ", ".join(line))
        print(f"\nproperties: {len(tasks) - bad}/{len(tasks)} references pass; "
              f"{caught}/{total} broken versions caught as contract_violation")
        return 0 if bad == 0 and caught == total else 1
    if not args.model:
        ap.error("--mode score requires --model")
    classes: dict[str, int] = {}
    by_t: dict[str, list[int]] = {}
    for t in tasks:
        try:
            code = sanitize(query_model(t["prompt"], args.endpoint, args.model,
                                        what=f"the one Aether function `{t['function']}` (no main)"))
            cls = run_property(t, code) if code.strip() else "no_answer"
        except Exception as exc:  # noqa: BLE001
            cls = f"infra_failed:{type(exc).__name__}"
        classes[cls] = classes.get(cls, 0) + 1
        by_t.setdefault(t["template"], []).append(int(cls == "pass"))
        print(f"[{'PASS' if cls == 'pass' else 'fail'}] {t['id']:<22} {cls}")
    passed = classes.get("pass", 0)
    print(f"\nproperty score: {passed}/{len(tasks)} ({passed / len(tasks):.0%}); "
          + ", ".join(f"{k} {v}" for k, v in sorted(classes.items())))
    for k, v in sorted(by_t.items()):
        print(f"  {k:<14} {sum(v)}/{len(v)}")
    return 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--mode", choices=["validate", "score", "negatives"], default="validate")
    ap.add_argument("--endpoint", default="http://localhost:8019/v1/chat/completions")
    ap.add_argument("--model", default=None, help="served model name (required for --mode score)")
    ap.add_argument("--suite", choices=["values", "properties"], default="values",
                    help="values: tasks with an expected output (default); properties: one function "
                         "checked by @post properties on generated inputs (validate and score)")
    ap.add_argument("--paraphrases", type=int, default=1, metavar="N",
                    help=f"ask each task in N wordings (1-{MAX_PARAPHRASES}; validate and score)")
    ap.add_argument("--dump", default=None, help="write generated tasks (+oracles) to this JSON path; with --mode negatives, "
                         "the kept should_fail items as a bench manifest")
    args = ap.parse_args()

    if not 1 <= args.paraphrases <= MAX_PARAPHRASES:
        ap.error(f"--paraphrases must be 1-{MAX_PARAPHRASES}")
    if args.suite == "properties":
        raise SystemExit(properties_main(args, ap))
    tasks = generate(args.n, args.seed, args.paraphrases)
    if args.mode == "negatives":
        kept, dropped = generate_negatives(tasks, args.seed)
        for item in dropped:
            print(f"[drop] {item['id']:<36} wanted {item['expected_error_code']}, got rc={item['observed_rc']} "
                  f"codes={item['observed_codes'] or 'none'}: {item['observed_first_line']}")
        per: dict[str, int] = {}
        for item in kept:
            per[item["source_task"]] = per.get(item["source_task"], 0) + 1
        for t in tasks:
            print(f"[{'ok ' if per.get(t['id'], 0) >= 2 else 'LOW'}] {t['id']:<16} {per.get(t['id'], 0)} kept")
        print(f"\nnegatives: kept {len(kept)}/{len(kept) + len(dropped)} broken programs "
              f"from {len(tasks)} tasks (seed {args.seed})")
        if args.dump:
            with open(args.dump, "w", encoding="utf-8") as fh:
                json.dump({"seed": args.seed, "generator": "tools/aether_wild_eval.py --mode negatives",
                           "tasks": kept}, fh, indent=2)
                fh.write("\n")
        raise SystemExit(0 if all(per.get(t["id"], 0) >= 2 for t in tasks) else 1)
    if args.dump:
        with open(args.dump, "w", encoding="utf-8") as fh:
            json.dump({"seed": args.seed, "tasks": tasks}, fh, indent=2)

    if args.mode == "validate":
        ok = 0
        for t in tasks:
            rc, out = run_aether(t["reference"])
            good = rc == 0 and out == t["expected_stdout"]
            ok += good
            flag = "ok " if good else "BAD"
            print(f"[{flag}] {t['id']:<16} oracle={t['expected_stdout']!r}"
                  + ("" if good else f"  got rc={rc} out={out!r}"))
        print(f"\nself-validation: {ok}/{len(tasks)} reference solutions match the Python oracle")
        if args.paraphrases > 1:
            bad = 0
            for t in tasks:
                for i, wording in enumerate(t["paraphrases"]):
                    gone = missing_facts(wording, t["facts"])
                    bad += bool(gone)
                    if gone:
                        print(f"[BAD] {t['id']} wording {i} leaves out {gone}")
            total = len(tasks) * args.paraphrases
            print(f"wordings: {total - bad}/{total} state every value and the output format "
                  f"the one reference answer needs")
            if bad:
                raise SystemExit(1)
        return

    if not args.model:
        ap.error("--mode score requires --model")
    passed = 0
    by_t: dict[str, list[int]] = {}
    verdicts: dict[str, list[int]] = {}
    for t in tasks:
        for i, prompt in enumerate(t.get("paraphrases") or [t["prompt"]]):
            try:
                code = sanitize(query_model(prompt, args.endpoint, args.model))
                rc, out = (run_aether(code) if code.strip() else (-1, ""))
            except Exception as exc:  # noqa: BLE001
                rc, out = -2, f"<{type(exc).__name__}>"
            good = rc == 0 and out == t["expected_stdout"]
            passed += good
            by_t.setdefault(t["template"], []).append(int(good))
            verdicts.setdefault(t["id"], []).append(int(good))
            label = t["id"] if args.paraphrases == 1 else f"{t['id']}/w{i}"
            print(f"[{'PASS' if good else 'fail'}] {label:<16} rc={rc}")
    calls = len(tasks) * args.paraphrases
    print(f"\nwild score: {passed}/{calls} ({passed / calls:.0%}) on novel tasks")
    for k, v in sorted(by_t.items()):
        print(f"  {k:<14} {sum(v)}/{len(v)}")
    if args.paraphrases > 1:
        print(f"\nmodel calls: {calls} ({len(tasks)} tasks x {args.paraphrases} wordings) to {args.model}")
        for line in consistency_report(verdicts, {t["id"]: t["template"] for t in tasks}):
            print(line)


if __name__ == "__main__":
    main()
