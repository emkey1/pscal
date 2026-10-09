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
            never-before-seen tasks. --paraphrases N asks each task in N
            wordings (every template carries five) and reports, per task and
            per template, whether the verdict holds across them: at
            temperature 0 a verdict that changes with the wording was caused
            by the wording. N=1 is the original prompt alone. Costs
            tasks x N model calls.

  negatives (CPU-only) -- break each generated reference in the ways the
            compiler must refuse (MUTATORS: println outside fx, fx inside
            @pure, `return`, clamp with two arguments, a misspelled type, a
            failing @pre), run every broken program under --deny net,proc and
            keep only those that fail with exactly the expected code. With
            --dump, writes them as `should_fail` entries in the benchmark's
            task format, for tools/aether_oracle_check.py --tasks FILE.

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


# ---- templates: each returns dict(template, prompt, paraphrases, expected_stdout, reference,
# mechanisms). `paraphrases` rewords `prompt` and asks for the same output, so one reference
# answer checks every wording; building them draws nothing from rng. ----
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
    xs = ', '.join(map(str, vals))
    prompt = (f"Print the arithmetic mean of these {k} integers, rounded to exactly two "
              f"decimal places, as `mean = <value>`: {xs}.")
    paraphrases = [
        f"Compute the average of {xs} and print it as `mean = <value>`, with exactly two digits "
        f"after the decimal point.",
        f"Given the integers {xs}, output a single line `mean = <value>` where <value> is their "
        f"mean rounded to two decimal places.",
        f"Write a program that prints `mean = <value>`, the mean of the {k} numbers {xs}, "
        f"formatted with exactly 2 decimal places.",
        f"What is the arithmetic mean of {xs}? Print it as `mean = <value>`, rounded to two decimals.",
    ]
    return dict(template="mean", prompt=prompt, paraphrases=paraphrases, expected_stdout=expected,
                reference=ref, mechanisms=["real", "arithmetic"])


def t_sum(rng: random.Random) -> dict:
    k = rng.choice([3, 4, 5, 6])
    vals = [rng.randint(1, 50) for _ in range(k)]
    expected = f"sum = {sum(vals)}\n"
    ref = (f"fn main() -> Void {{\n    let s: Int = {' + '.join(map(str, vals))};\n"
           f"    fx {{ println(\"sum = \", s); }}\n    ret;\n}}\n")
    xs = ', '.join(map(str, vals))
    prompt = f"Print the sum of these integers as `sum = <value>`: {xs}."
    paraphrases = [
        f"Add up {xs} and print the total as `sum = <value>`.",
        f"Output one line, `sum = <value>`, where <value> is the total of these integers: {xs}.",
        f"Write a program that prints the sum of {xs} in the form `sum = <value>`.",
        f"What do the integers {xs} add up to? Print it as `sum = <value>`.",
    ]
    return dict(template="sum", prompt=prompt, paraphrases=paraphrases, expected_stdout=expected,
                reference=ref, mechanisms=["arithmetic"])


def t_clamp(rng: random.Random) -> dict:
    lo = rng.randint(0, 20)
    hi = lo + rng.randint(40, 80)
    val = rng.randint(lo - 30, hi + 30)
    expected = f"{max(lo, min(hi, val))}\n"
    ref = f"fn main() -> Void {{\n    fx {{ println(clamp({val}, {lo}, {hi})); }}\n    ret;\n}}\n"
    prompt = f"Print {val} clamped to the inclusive range {lo} to {hi}."
    paraphrases = [
        f"Clamp the value {val} so it lies between {lo} and {hi} inclusive, and print the result.",
        f"Print {val} limited to the closed interval [{lo}, {hi}].",
        f"Write a program that prints the result of clamping {val} to the inclusive bounds {lo} and {hi}.",
        f"If {val} is outside the range {lo} to {hi} (both ends included), print the nearest bound; "
        f"otherwise print {val}.",
    ]
    return dict(template="clamp", prompt=prompt, paraphrases=paraphrases, expected_stdout=expected,
                reference=ref, mechanisms=["clamp", "builtin"])


def t_max(rng: random.Random) -> dict:
    k = rng.choice([3, 4, 5])
    vals = [rng.randint(0, 200) for _ in range(k)]
    expected = f"max = {max(vals)}\n"
    nested = _nest("mx", [str(v) for v in vals])
    ref = ("@pure\nfn mx(a: Int, b: Int) -> Int {\n    if a > b { ret a; }\n    ret b;\n}\n"
           f"fn main() -> Void {{\n    let m: Int = {nested};\n"
           f"    fx {{ println(\"max = \", m); }}\n    ret;\n}}\n")
    xs = ', '.join(map(str, vals))
    prompt = f"Print the largest of these integers as `max = <value>`: {xs}."
    paraphrases = [
        f"Find the maximum of {xs} and print it as `max = <value>`.",
        f"Output one line, `max = <value>`, where <value> is the greatest of these integers: {xs}.",
        f"Write a program that prints the biggest number among {xs} in the form `max = <value>`.",
        f"Which of {xs} is largest? Print it as `max = <value>`.",
    ]
    return dict(template="max", prompt=prompt, paraphrases=paraphrases, expected_stdout=expected,
                reference=ref, mechanisms=["branching", "pure"])


def t_count_above(rng: random.Random) -> dict:
    k = rng.choice([4, 5, 6])
    vals = [rng.randint(0, 100) for _ in range(k)]
    thr = rng.randint(30, 70)
    expected = f"{sum(1 for v in vals if v >= thr)}\n"
    terms = " + ".join(f"ge({v}, {thr})" for v in vals)
    ref = ("@pure\nfn ge(v: Int, t: Int) -> Int {\n    if v >= t { ret 1; }\n    ret 0;\n}\n"
           f"fn main() -> Void {{\n    let c: Int = {terms};\n"
           f"    fx {{ println(c); }}\n    ret;\n}}\n")
    xs = ', '.join(map(str, vals))
    prompt = (f"Count how many of these integers are greater than or equal to {thr}, and print "
              f"just that count: {xs}.")
    paraphrases = [
        f"Print the number of values in {xs} that are at least {thr}.",
        f"How many of the integers {xs} are {thr} or more? Print only that number.",
        f"Write a program that counts the entries of {xs} that are >= {thr} and prints the count alone.",
        f"Given the threshold {thr} and the integers {xs}, print just the count of integers not "
        f"below the threshold.",
    ]
    return dict(template="count_above", prompt=prompt, paraphrases=paraphrases, expected_stdout=expected,
                reference=ref, mechanisms=["pure", "branching"])


TEMPLATES = [t_mean, t_sum, t_clamp, t_max, t_count_above]


def generate(n: int, seed: int) -> list[dict]:
    rng = random.Random(seed)
    out = []
    for i in range(n):
        tmpl = TEMPLATES[i % len(TEMPLATES)]
        task = tmpl(rng)
        task["id"] = f"{task['template']}_{i:03d}"
        out.append(task)
    return out


def run_aether_full(source: str, flags: tuple[str, ...] = ()) -> tuple[int, str, str]:
    with tempfile.TemporaryDirectory(prefix="wild-eval-") as td:
        p = os.path.join(td, "prog.aether")
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(source)
        proc = subprocess.run([AETHER_BIN, "--no-cache", *flags, p], cwd=td,
                              capture_output=True, text=True, errors="replace", timeout=30)
        return proc.returncode, proc.stdout, proc.stderr


def run_aether(source: str) -> tuple[int, str]:
    rc, out, _ = run_aether_full(source)
    return rc, out


# ---- should-fail mutants: each breaks one rule in a reference, or returns None ----
_FX_PRINTLN = re.compile(r"fx \{ (println\(.*\);) \}")
_PURE_FN = re.compile(r"@pure\nfn (\w+)\((\w+): Int[^\n]*\{\n")


def m_println_outside_fx(ref: str) -> str | None:
    out = _FX_PRINTLN.sub(r"\1", ref, count=1)
    return out if out != ref else None


def m_fx_in_pure(ref: str) -> str | None:
    m = _PURE_FN.search(ref)
    if m:
        return ref[:m.end()] + "    fx { println(\"in pure\"); }\n" + ref[m.end():]
    # No pure helper of its own: add one that prints.
    return ("@pure\nfn note(v: Int) -> Int {\n    fx { println(v); }\n    ret v;\n}\n" + ref)


def m_return_for_ret(ref: str) -> str | None:
    i = ref.rfind("ret;")
    return ref[:i] + "return;" + ref[i + len("ret;"):] if i != -1 else None


def m_clamp_two_args(ref: str) -> str | None:
    out = re.sub(r"clamp\(([^,()]+), ([^,()]+), ([^,()]+)\)", r"clamp(\1, \3)", ref, count=1)
    return out if out != ref else None


def m_misspelled_type(ref: str) -> str | None:
    out = re.sub(r"(let \w+: )Int\b", r"\1Strng", ref, count=1)
    return out if out != ref else None


def m_failing_contract(ref: str) -> str | None:
    # Every template draws its values from 0..200, so this @pre never holds.
    m = _PURE_FN.search(ref)
    if not m:
        return None
    return ref[:m.start()] + f"@pre {m.group(2)} > 1000\n" + ref[m.start():]


# (rule, expected code, mutator). NARROW-001 is a warning (exit 0): a trap, not a negative.
MUTATORS = [
    ("println_outside_fx", "FX-001", m_println_outside_fx),
    ("fx_in_pure", "ANN-001", m_fx_in_pure),
    ("return_for_ret", "SYN-001", m_return_for_ret),
    ("clamp_two_args", "BUILT-002", m_clamp_two_args),
    ("misspelled_type", "TYPE-002", m_misspelled_type),
    ("failing_contract", "CON-001", m_failing_contract),
]
NEGATIVE_FLAGS = ("--deny", "net,proc")  # the harness's sandbox, as the oracle check runs it
_CODE = re.compile(r"\b[A-Z]+-\d{3}\b")


def negatives(tasks: list[dict]) -> list[dict]:
    """Every mutant of every task's reference, as a should_fail entry."""
    out = []
    for t in tasks:
        for rule, code, mutate in MUTATORS:
            program = mutate(t["reference"])
            if program is None:
                continue
            out.append({"id": f"{t['id']}__{rule}", "title": f"Compiler rejects {rule.replace('_', ' ')}",
                        "should_fail": True, "program": program, "expected_error_code": code,
                        "mechanisms": ["negative", "generated", rule], "template": t["template"],
                        "note": f"aether_wild_eval.py --mode negatives: {t['id']}'s reference with {rule}."})
    return out


def check_negative(item: dict, run=run_aether_full) -> tuple[bool, str]:
    """(kept, why): a mutant is kept only if it fails with exactly its code."""
    rc, out, err = run(item["program"], NEGATIVE_FLAGS)
    codes = sorted(set(_CODE.findall(out + err)))
    if rc == 0:
        return False, "exit 0"
    if codes != [item["expected_error_code"]]:
        return False, f"codes {codes or 'none'}"
    return True, f"rc={rc}"


def wordings(task: dict) -> list[str]:
    return [task["prompt"], *task.get("paraphrases", [])]


def score(tasks: list[dict], n: int, ask, run=run_aether) -> list[dict]:
    """Ask each task in its first n wordings; one verdict per wording, all
    against the task's one expected_stdout."""
    results = []
    for t in tasks:
        verdicts, rcs = [], []
        for prompt in wordings(t)[:n]:
            try:
                code = sanitize(ask(prompt))
                rc, out = (run(code) if code.strip() else (-1, ""))
            except Exception as exc:  # noqa: BLE001
                rc, out = -2, f"<{type(exc).__name__}>"
            verdicts.append(rc == 0 and out == t["expected_stdout"])
            rcs.append(rc)
        results.append({"id": t["id"], "template": t["template"], "verdicts": verdicts, "rcs": rcs})
    return results


def report_score(results: list[dict], n: int) -> list[str]:
    lines: list[str] = []
    if n == 1:
        for r in results:
            lines.append(f"[{'PASS' if r['verdicts'][0] else 'fail'}] {r['id']:<16} rc={r['rcs'][0]}")
    else:
        for r in results:
            v = r["verdicts"]
            flips = sum(1 for x in v[1:] if x != v[0])
            lines.append(f"[{''.join('P' if x else '.' for x in v)}] {r['id']:<16} pass {sum(v)}/{n}  "
                         + ("consistent" if flips == 0 else f"{flips}/{n - 1} wording(s) flip the verdict"))
    passed = sum(1 for r in results if r["verdicts"][0])
    lines.append(f"\nwild score: {passed}/{len(results)} ({passed / len(results):.0%}) on novel tasks")
    by_t: dict[str, list[dict]] = {}
    for r in results:
        by_t.setdefault(r["template"], []).append(r)
    for k, rs in sorted(by_t.items()):
        line = f"  {k:<14} {sum(1 for r in rs if r['verdicts'][0])}/{len(rs)}"
        if n > 1:
            all_v = [x for r in rs for x in r["verdicts"]]
            same = sum(1 for r in rs if len(set(r["verdicts"])) == 1)
            line += f"  all wordings {sum(all_v)}/{len(all_v)}  same verdict in every wording {same}/{len(rs)}"
        lines.append(line)
    if n > 1:
        same = sum(1 for r in results if len(set(r["verdicts"])) == 1)
        lines.append(f"rewording: {same}/{len(results)} tasks keep one verdict across {n} wordings")
    lines.append(f"model calls: {len(results)} tasks x {n} wording(s) = {len(results) * n}")
    return lines


def query_model(prompt: str, endpoint: str, model: str) -> str:
    full = (f"You are writing Aether code. Write exactly one complete Aether program for the "
            f"task below. Return only raw Aether source code, no Markdown fences, no explanation. "
            f"After the program, output a final line containing exactly {END_MARKER}.\n\nTask:\n{prompt}")
    body = json.dumps({"model": model, "messages": [{"role": "user", "content": full}],
                       "max_tokens": 500, "temperature": 0}).encode()
    req = urllib.request.Request(endpoint, data=body, headers={"Content-Type": "application/json"})
    resp = json.loads(urllib.request.urlopen(req, timeout=120).read().decode("utf-8", "replace"))
    return resp["choices"][0]["message"]["content"]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--mode", choices=["validate", "score", "negatives"], default="validate")
    ap.add_argument("--endpoint", default="http://localhost:8019/v1/chat/completions")
    ap.add_argument("--model", default=None, help="served model name (required for --mode score)")
    ap.add_argument("--paraphrases", type=int, default=1, metavar="N",
                    help="--mode score: ask each task in its first N wordings (1-5; 1 = the original only)")
    ap.add_argument("--dump", default=None, help="write generated tasks (+oracles) to this JSON path; "
                    "with --mode negatives, the kept should_fail entries")
    args = ap.parse_args()

    tasks = generate(args.n, args.seed)
    if args.mode == "negatives":
        kept: list[dict] = []
        by_t: dict[str, list[int]] = {}
        for item in negatives(tasks):
            ok, why = check_negative(item)
            by_t.setdefault(item["template"], []).append(int(ok))
            if ok:
                kept.append(item)
            print(f"[{'keep' if ok else 'drop'}] {item['id']:<36} {item['expected_error_code']:<10} {why}")
        print(f"\nnegatives: kept {len(kept)}/{sum(len(v) for v in by_t.values())} mutants "
              f"that fail with exactly their code")
        for k, v in sorted(by_t.items()):
            print(f"  {k:<14} {sum(v)}/{len(v)}")
        if args.dump:
            with open(args.dump, "w", encoding="utf-8") as fh:
                json.dump({"version": f"wild-negatives-seed{args.seed}-n{args.n}", "seed": args.seed,
                           "tasks": kept}, fh, indent=2)
                fh.write("\n")
        if any(sum(v) < 2 for v in by_t.values()):
            raise SystemExit("a template yielded fewer than two kept negatives")
        return
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
        return

    if not args.model:
        ap.error("--mode score requires --model")
    most = min(len(wordings(t)) for t in tasks)
    if not 1 <= args.paraphrases <= most:
        ap.error(f"--paraphrases must be 1..{most}")
    results = score(tasks, args.paraphrases, lambda prompt: query_model(prompt, args.endpoint, args.model))
    for line in report_score(results, args.paraphrases):
        print(line)


if __name__ == "__main__":
    main()
