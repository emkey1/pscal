#!/usr/bin/env python3
"""Aether as verifier: a model's candidate is accepted or rejected by running it under contracts, never by the
model's own say-so.

Small models check their own work reliably only when the checking is handed to a tool (GenCorrect, SETS,
tool-integrated self-verification). This is that loop for Aether:

1. The model gets a fixed signature and a plain-English spec. It writes the function, with its own @pre/@post
   contracts saying what callers must supply and what it guarantees.
2. The harness puts the task's trusted @post properties above the function, as the property suite in
   aether_wild_eval.py does, and appends a main that calls it on generated inputs. The compiler runs it. A
   failed property or contract stops the program with [CON-001] (contract_violation). The verdict is
   whatever the run says.
3. On a reject, the model is shown the failure (CON-001 line, compile error, wrong output) and asked again,
   up to --rounds times.

Two more measurements, because they are the point of the exercise:
* self-grade: the model is asked whether its first candidate meets the spec (YES/NO). The report sets that
  answer against the execution verdict. A YES the run rejects is a false accept.
* own contracts: the model's own @pre/@post, taken alone, are run against the reference (they must accept it)
  and against the template's known-broken versions (how many they catch). This shows whether the model's
  contracts would work as a verifier on their own.

Templates: the five property templates from aether_wild_eval.py plus floor_div and round_up_to.

    # no model: references pass, broken versions are caught, the contract-strength machinery works
    AETHER_BIN=... python3 tools/aether_verify_loop.py --mode validate --n 14 --seed 3

    # a model through any OpenAI-style endpoint (the m4 proxy, OpenRouter, a local server)
    AETHER_BIN=... python3 tools/aether_verify_loop.py --mode score --n 14 --rounds 3 \\
        --endpoint http://m4mini.tailfe3968.ts.net:8788/v1/chat/completions --model small --out run.json
    # OpenRouter: --endpoint https://openrouter.ai/api/v1/chat/completions --model <id> \\
    #             --api-key-env OPENROUTER_API_KEY [--env-file path/to/.env]   (the key is never printed)

Needs a compiler at language 2026-10-15-2 or later (CON-001).
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aether_wild_eval as wild  # noqa: E402


# ---- two templates beyond the property suite's five ----
def p_floor_div(rng: random.Random) -> dict:
    b = rng.randint(2, 9)
    q = rng.randint(1, 30)
    as_ = [0, b - 1, q * b, q * b + rng.randint(1, b - 1), rng.randint(0, 999), b]
    sig = "fn floor_div(a: Int, b: Int) -> Int"
    return wild._prop("floor_div", sig, "For a >= 0 and b > 0 it returns how many whole times b fits into a "
                      "(a divided by b, rounded down).",
                      ["result >= 0", "result * b <= a", "(result + 1) * b > a"],
                      [(a, b) for a in as_],
                      f"@pure\n{sig} {{\n    let q: Int = 0;\n    let r: Int = a;\n    while r >= b {{\n"
                      f"        r = r - b;\n        q = q + 1;\n    }}\n    ret q;\n}}\n",
                      {"rounds_up": f"{sig} {{\n    ret (a + b - 1) / b;\n}}\n",
                       "plus_one": f"{sig} {{\n    ret a / b + 1;\n}}\n",
                       "returns_a": f"{sig} {{\n    ret a;\n}}\n"},
                      ["contract", "loops"])


def p_round_up_to(rng: random.Random) -> dict:
    m = rng.randint(2, 12)
    k = rng.randint(1, 20)
    vs = [0, k * m, k * m + 1, k * m - 1, rng.randint(0, 500), m]
    sig = "fn round_up_to(v: Int, m: Int) -> Int"
    return wild._prop("round_up_to", sig, "For v >= 0 and m > 0 it returns the smallest multiple of m that is "
                      "at least v.",
                      ["result >= v", "result % m == 0", "result - m < v"],
                      [(v, m) for v in vs],
                      f"@pure\n{sig} {{\n    if v % m == 0 {{ ret v; }}\n    ret v + m - v % m;\n}}\n",
                      {"always_adds": f"{sig} {{\n    ret v + m - v % m;\n}}\n",
                       "rounds_down": f"{sig} {{\n    ret v - v % m;\n}}\n"},
                      ["contract", "branching"])


TEMPLATES = list(wild.PROPERTY_TEMPLATES) + [p_floor_div, p_round_up_to]


def generate(n: int, seed: int) -> list[dict]:
    rng = random.Random(f"verify:{seed}")
    out = []
    for i in range(n):
        task = TEMPLATES[i % len(TEMPLATES)](rng)
        task["id"] = f"ver_{task['template']}_{i:03d}"
        out.append(task)
    return out


# ---- running a candidate: the verdict comes from execution ----
_FAIL_LINE = re.compile(r"\[[A-Z]+-\d{3}\].*|.*(?:error|Error|failed).*")


def judge(task: dict, code: str, run=None) -> tuple[str, str]:
    """(class, what went wrong) for one candidate under the task's trusted properties."""
    import aether_doc_bench as adb  # the bench's classifier, as the property suite uses it
    run = run or wild.run_aether_full
    rc, out, err = run(wild.wrap_property(task, code))
    cls = adb.classify_attempt({"generated_ok": True, "source_code": code, "run": {
        "returncode": rc, "stdout": out, "stderr": err, "expected_returncode": 0,
        "exact_stdout_match": rc == 0 and out == task["expected_stdout"]}})
    if cls == "pass":
        return cls, ""
    lines = [ln.strip() for ln in (err or "").splitlines() if _FAIL_LINE.match(ln.strip())]
    detail = "; ".join(lines[:3]) or (f"it printed {out.strip()[:200]!r} instead of "
                                      f"{task['expected_stdout'].strip()!r}" if rc == 0 else f"exit code {rc}")
    return cls, detail[:600]


def own_contracts(code: str, function: str) -> list[str]:
    """The model's own @pre/@post lines directly above `fn <function>(` (other annotations skipped)."""
    lines = wild._drop_main(code).splitlines()
    head = next((i for i, ln in enumerate(lines) if re.match(rf"\s*fn {re.escape(function)}\s*\(", ln)), None)
    if head is None:
        return []
    out = []
    i = head - 1
    while i >= 0 and lines[i].lstrip().startswith("@"):
        if re.match(r"\s*@(pre|post)\b", lines[i]):
            out.insert(0, lines[i].strip())
        i -= 1
    return out


def contract_strength(task: dict, contracts: list[str], run=None) -> dict:
    """The model's own contracts as the only check: do they accept the reference, and how many of the
    template's broken versions do they catch?"""
    if not contracts:
        return {"contracts": 0}
    bare = dict(task, posts=[])
    head = "\n".join(contracts) + "\n"
    ref_cls, _ = judge(bare, head + task["reference"], run)
    caught = {name: judge(bare, head + src, run)[0] == "contract_violation" for name, src in task["broken"].items()}
    return {"contracts": len(contracts), "accepts_reference": ref_cls == "pass", "reference_class": ref_cls,
            "caught": sum(caught.values()), "broken": len(caught),
            "missed": sorted(k for k, v in caught.items() if not v)}


# ---- the model side ----
def chat(messages: list[dict], endpoint: str, model: str, api_key: str = "", max_tokens: int = 900,
         timeout: float = 180) -> str:
    body = json.dumps({"model": model, "messages": messages, "max_tokens": max_tokens,
                       "temperature": 0}).encode()
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    req = urllib.request.Request(endpoint, data=body, headers=headers)
    resp = json.loads(urllib.request.urlopen(req, timeout=timeout).read().decode("utf-8", "replace"))
    return resp["choices"][0]["message"]["content"] or ""


def generator_prompt(task: dict) -> str:
    return (f"You are writing Aether code. Write the one Aether function `{task['signature']}` - exactly this "
            f"signature, and no main. {task['prompt'].split('`. ', 1)[-1]}\n\n"
            "Directly above the function, put @pre and @post contract lines, one condition per line, such as "
            "`@pre(n >= 0)` and `@post(result >= 0)`: @pre says what callers must supply, @post what the function "
            "guarantees (`result` is the returned value). They are checked every time the function runs, so make "
            "the @post lines as strong as the spec allows: together they should reject any wrong answer.\n\n"
            f"Return only raw Aether source code, no Markdown fences, no explanation. After the code, output a "
            f"final line containing exactly {wild.END_MARKER}.")


def repair_prompt(cls: str, detail: str) -> str:
    return (f"That function was compiled and run on test inputs under the spec's checks, and it was rejected "
            f"({cls}): {detail or 'no further detail'}.\nFix it. Return the whole corrected function with its "
            f"contracts, raw Aether only, then a final line containing exactly {wild.END_MARKER}.")


def self_grade_prompt(task: dict, code: str) -> str:
    return (f"Spec: `{task['signature']}`. {task['prompt'].split('`. ', 1)[-1]}\n\nCandidate:\n{code}\n\n"
            "Does this candidate meet the spec for every valid input? Answer with exactly one word: YES or NO.")


def parse_grade(text: str) -> str:
    text = wild.sanitize(text).strip().upper()
    m = re.search(r"\b(YES|NO)\b", text)
    return m.group(1).lower() if m else "unclear"


def verify_task(task: dict, ask, rounds: int = 3, self_grade: bool = True, run=None) -> dict:
    """One task through the loop. `ask(messages) -> reply text` is the model."""
    messages = [{"role": "user", "content": generator_prompt(task)}]
    tries = []
    for r in range(1, rounds + 1):
        reply = ask(messages)
        code = wild.sanitize(reply)
        cls, detail = judge(task, code, run) if code.strip() else ("no_answer", "the reply had no code")
        tries.append({"round": r, "class": cls, "detail": detail, "code": code})
        if cls == "pass":
            break
        messages += [{"role": "assistant", "content": reply}, {"role": "user", "content": repair_prompt(cls, detail)}]
    first, last = tries[0], tries[-1]
    out = {"id": task["id"], "template": task["template"], "first": first["class"], "final": last["class"],
           "rounds": len(tries), "tries": tries}
    if self_grade and first["code"].strip():
        grade = parse_grade(ask([{"role": "user", "content": self_grade_prompt(task, first["code"])}]))
        out["self_grade"] = grade
        out["self_grade_agrees"] = (grade == "yes") == (first["class"] == "pass") if grade != "unclear" else None
    out["own_contracts"] = contract_strength(task, own_contracts(last["code"], task["function"]), run)
    return out


def summarize(results: list[dict]) -> dict:
    n = len(results)
    graded = [r for r in results if r.get("self_grade") in ("yes", "no")]
    with_contracts = [r["own_contracts"] for r in results if r["own_contracts"].get("contracts")]
    classes: dict[str, int] = {}
    for r in results:
        classes[r["final"]] = classes.get(r["final"], 0) + 1
    return {
        "tasks": n,
        "accepted_first_try": sum(r["first"] == "pass" for r in results),
        "accepted_after_loop": sum(r["final"] == "pass" for r in results),
        "model_calls": sum(r["rounds"] for r in results) + sum("self_grade" in r for r in results),
        "final_classes": classes,
        "self_grade": {"answered": len(graded),
                       "agrees_with_execution": sum(bool(r["self_grade_agrees"]) for r in graded),
                       "false_accepts": sum(r["self_grade"] == "yes" and r["first"] != "pass" for r in graded),
                       "false_rejects": sum(r["self_grade"] == "no" and r["first"] == "pass" for r in graded)},
        "own_contracts": {"written": len(with_contracts),
                          "accept_reference": sum(c["accepts_reference"] for c in with_contracts),
                          "broken_caught": sum(c["caught"] for c in with_contracts),
                          "broken_total": sum(c["broken"] for c in with_contracts)},
    }


def report_lines(summary: dict) -> list[str]:
    s, sg, oc = summary, summary["self_grade"], summary["own_contracts"]
    return [f"accepted by execution: {s['accepted_first_try']}/{s['tasks']} first try, "
            f"{s['accepted_after_loop']}/{s['tasks']} after the repair loop ({s['model_calls']} model calls)",
            "final classes: " + ", ".join(f"{k} {v}" for k, v in sorted(s["final_classes"].items())),
            f"self-grade vs execution: {sg['agrees_with_execution']}/{sg['answered']} agree; "
            f"{sg['false_accepts']} false accepts (said YES, run rejected), {sg['false_rejects']} false rejects",
            f"own contracts: written on {oc['written']}/{s['tasks']}; accept the reference on "
            f"{oc['accept_reference']}/{oc['written']}; catch {oc['broken_caught']}/{oc['broken_total']} broken versions"]


def _api_key(name: str, env_file: str) -> str:
    """The key named `name`, from the environment or one line of env_file; never printed."""
    if not name:
        return ""
    if os.environ.get(name):
        return os.environ[name]
    if env_file and os.path.exists(env_file):
        with open(env_file, encoding="utf-8") as fh:
            for line in fh:
                m = re.match(rf"\s*(?:export\s+)?{re.escape(name)}\s*=\s*(.+)$", line)
                if m:
                    return m.group(1).strip().strip("'\"")
    raise SystemExit(f"{name} is not set (environment or --env-file)")


def validate(tasks: list[dict]) -> int:
    """No model: every reference passes, every broken version is caught, and the contract-strength check tells
    a strong set of contracts (the task's own properties) from a vacuous one."""
    bad = 0
    for t in tasks:
        ref, _ = judge(t, t["reference"])
        broken = {k: judge(t, src)[0] for k, src in t["broken"].items()}
        strong = contract_strength(t, [f"@post({p})" for p in t["posts"]])
        vacuous = contract_strength(t, ["@post(result == result)"])
        ok = (ref == "pass" and all(c == "contract_violation" for c in broken.values())
              and strong["accepts_reference"] and strong["caught"] == strong["broken"]
              and vacuous["accepts_reference"] and vacuous["caught"] == 0)
        bad += not ok
        print(f"[{'ok ' if ok else 'BAD'}] {t['id']:<24} reference {ref}; broken "
              + ", ".join(f"{k} {c}" for k, c in broken.items())
              + f"; strong contracts catch {strong['caught']}/{strong['broken']}, vacuous {vacuous['caught']}")
    print(f"\nvalidate: {len(tasks) - bad}/{len(tasks)} tasks ok")
    return 0 if bad == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--mode", choices=["validate", "score"], default="validate")
    ap.add_argument("--n", type=int, default=14)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rounds", type=int, default=3, help="candidate + repairs per task (default 3)")
    ap.add_argument("--no-self-grade", action="store_true", help="skip asking the model to grade itself")
    ap.add_argument("--endpoint", default="http://localhost:8019/v1/chat/completions")
    ap.add_argument("--model", default=None)
    ap.add_argument("--api-key-env", default="", help="name of the variable holding the endpoint's key")
    ap.add_argument("--env-file", default="", help="file to read --api-key-env from if it is not in the environment")
    ap.add_argument("--max-tokens", type=int, default=900)
    ap.add_argument("--out", default="", help="write every try and the summary as JSON")
    args = ap.parse_args()
    tasks = generate(args.n, args.seed)
    if args.mode == "validate":
        return validate(tasks)
    if not args.model:
        ap.error("--mode score requires --model")
    key = _api_key(args.api_key_env, args.env_file)

    def ask(messages):
        return chat(messages, args.endpoint, args.model, key, args.max_tokens)

    results = []
    started = time.time()
    for t in tasks:
        try:
            r = verify_task(t, ask, rounds=args.rounds, self_grade=not args.no_self_grade)
        except Exception as exc:  # noqa: BLE001 - one failed call is one failed task, not a failed run
            r = {"id": t["id"], "template": t["template"], "first": f"infra_failed:{type(exc).__name__}",
                 "final": f"infra_failed:{type(exc).__name__}", "rounds": 0, "tries": [],
                 "own_contracts": {"contracts": 0}}
        results.append(r)
        oc = r["own_contracts"]
        print(f"[{'PASS' if r['final'] == 'pass' else 'fail'}] {r['id']:<24} first {r['first']}, final {r['final']} "
              f"in {r['rounds']}; self-grade {r.get('self_grade', '-')}; own contracts "
              + (f"{oc['caught']}/{oc['broken']} caught, reference {'ok' if oc['accepts_reference'] else 'REJECTED'}"
                 if oc.get("contracts") else "none"), flush=True)
    summary = summarize(results)
    print()
    for line in report_lines(summary):
        print(line)
    print(f"({time.time() - started:.0f} s, model {args.model})")
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump({"model": args.model, "endpoint": args.endpoint, "seed": args.seed, "rounds": args.rounds,
                       "summary": summary, "results": results}, fh, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
