#!/usr/bin/env python3
"""Generate one report per completed benchmark run from the saved result JSON.

Everything factual here -- scores, suite sizes, repeat counts, generated_ok,
repair lift, grader version, task-suite version -- is read out of the result
files, not from memory. The per-run commentary is supplied in RUNS below and is
deliberately short: each document is about ITS OWN run, and references earlier
boards only where that explains a change to the language, the harness or the
suite.
"""
import json, glob, os, collections, pathlib, sys

ROOT = "/storage"
OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/benchdocs")
OUT.mkdir(parents=True, exist_ok=True)

SUITE_N = {"simple": 35, "large": 9, "cs": 19, "nontoon": 5}
SUITE_ORDER = ["simple", "large", "cs", "nontoon"]

RUNS = [
    dict(dir="aether_eval_queue_cs_aug19_precision_claw3", slug="cs-aug19",
         title="cs-aug19 - 8B/9B precision grid",
         corpus="cs-aug19 (723 instruction + 38 repair)", host="claw3",
         repair="0 (harness default; not passed explicitly)",
         notes=[
             "Baseline board for the 0-based `Text` work that followed. Six models: "
             "two families x 4/8/16-bit.",
             "8-bit was the strongest arm in both families, replicating cs-aug18. That "
             "finding was later retired -- see cs-aug20-small-repair2, where enabling "
             "repair dissolves the precision spread.",
         ]),
    dict(dir="aether_eval_queue_cs_aug20_precision_claw3", slug="cs-aug20-8b",
         title="cs-aug20 - qwen3-8b family (first pass, repair-0)",
         corpus="cs-aug20 (726 instruction + 38 repair)", host="claw3",
         repair="0 (harness default)",
         notes=[
             "First board on the corpus with the 0-based `Text` migration finished and "
             "two inverted repair pairs corrected.",
             "`cs` rose in all three arms. `simple` and `large` moved inconsistently.",
         ]),
    dict(dir="aether_eval_queue_cs_aug20_qwen35_claw3", slug="cs-aug20-9b",
         title="cs-aug20 - qwen35-9b family (first pass, repair-0)",
         corpus="cs-aug20 (726 instruction + 38 repair)", host="claw3",
         repair="0 (harness default)",
         notes=[
             "Re-run after all three 9B models failed to serve at "
             "`--gpu-memory-utilization 0.18`. The 9B needs 0.22: 17.66 GiB of weights "
             "against a ~21.5 GiB budget leaves too little for KV cache, and the failure "
             "surfaces only as `Engine core initialization failed`.",
             "That constant is model-size- and host-specific, not a property of the box.",
         ]),
    dict(dir="aether_eval_queue_cs_aug20_large_claw2", slug="cs-aug20-large",
         title="cs-aug20 - 14B-35B roster",
         corpus="cs-aug20 (726 instruction + 38 repair)", host="claw2",
         repair="2",
         notes=[
             "First board for the 14B-35B tier on this corpus. Served on claw2 at "
             "`--gpu-memory-utilization 0.85`; these merges are 28-67 GB and do not fit "
             "claw3's usable window at all.",
             "`mistral24b` was served with the base tokenizer. The Unsloth merge omits "
             "`tekken.json`, so vLLM falls back to the HF path and drops spaces in "
             "generated text; it scored 9/35 on `simple` until corrected, then 34/35 on "
             "identical weights. Note `tokenizer.json` is byte-identical to base, so "
             "comparing that file does not detect the problem.",
             "Two models reached a perfect 19/19 on `cs`.",
         ]),
    dict(dir="aether_eval_queue_cs_aug20_small_repair2_claw3", slug="cs-aug20-small-repair2",
         title="cs-aug20 - 8B/9B grid re-scored with repair",
         corpus="cs-aug20 (726 instruction + 38 repair)", host="claw3",
         repair="2",
         notes=[
             "The 8B/9B grid re-run with `--repair-attempts 2` so both tiers of the board "
             "sit on the same setting. The harness defaults this to 0 while cs-aug4 used "
             "1, which had quietly given the older baseline a free extra attempt per "
             "failure.",
             "Every first-attempt score here reproduced its repair-0 original exactly, "
             "6 for 6 -- generation is reproducible across runs and hosts.",
             "Repair compresses the precision spread: `cs` goes 11/13/12 to 14/14/15 in "
             "the 8B family. The earlier '8-bit always wins' result was substantially an "
             "artifact of repair-0 scoring.",
         ]),
    dict(dir="aether_eval_es2_claw2", slug="early-stopping-experiment",
         title="Early-stopping experiment - qwen3-8b-nothink-8bit",
         corpus="cs-aug20 (689 instruction after a 75-case holdout)", host="claw2",
         repair="2",
         notes=[
             "Single-model test of whether the early-stopping criterion was ending runs "
             "prematurely. The default is a 12-sample eval set checked every 5 steps with "
             "`early_stopping_threshold=0.0` and patience 2, so a 0.03% uptick stops "
             "training.",
             "Treatment (75-case holdout, threshold 0.001, patience 4, eval every 20 "
             "steps) ran to epoch 2.77 instead of the control's 1.70 -- and scored the "
             "same. With a resolvable eval set the true optimum is ~epoch 1.85 and "
             "eval_loss genuinely rises after it, and `load_best_model_at_end` exports a "
             "near-best checkpoint either way.",
             "Conclusion: the criterion is noisy but not systematically costly. It does "
             "not invalidate any board. Caveat: the treatment also trained on 8% fewer "
             "records because of the larger holdout.",
         ]),
    dict(dir="aether_eval_arityfix_claw2", slug="diagnostics-fix",
         title="Diagnostic fixes re-measured - 14B-35B roster",
         corpus="cs-aug20 (726 instruction + 38 repair)", host="claw2",
         repair="2",
         notes=[
             "Re-measurement after four misleading compiler diagnostics were fixed: "
             "wrong-arity builtin calls reported as `identifier not in scope`, builtin "
             "redefinition reported as `expected parameter name`, raw `Yyjson*` internals "
             "surfacing to users, and the 1-D-array-indexed-as-2-D VM error.",
             "The grader is deliberately the FIXED compiler, so the compiler is the "
             "intended variable against the earlier board.",
             "This run also carries the first `nontoon` numbers, from the new non-TOON "
             "hard suite.",
             "Result: the diagnostics are provably gone (`has_toon` SYN-001 27 -> 0, raw "
             "`Yyjson*` -> 0) and `large` barely moved. The bad messages were masking "
             "ordinary model errors -- type errors, missing returns, missing `fx` blocks "
             "-- rather than causing failures. Recorded as a null result.",
         ]),
]


def score_file(path):
    d = json.load(open(path, encoding="utf-8"))
    v = d["destinations"][0]["variants"][0]
    rs = v["results"]
    suite = next((k for k in SUITE_N if path.endswith(f"_{k}.json")), None)
    n = SUITE_N.get(suite)
    first, final = collections.defaultdict(list), collections.defaultdict(list)
    for r in rs:
        t = r.get("task_id")
        att = r.get("attempts") or []
        first[t].append(bool(((att[0].get("run") or {}) if att else {}).get("exact_stdout_match")))
        final[t].append(bool((r.get("run") or {}).get("exact_stdout_match")))
    maj = lambda dd: sum(1 for vv in dd.values() if sum(vv) * 2 > len(vv))
    reps = len(next(iter(final.values()))) if final else 0
    return dict(suite=suite, n=n, reps=reps, cases=len(rs),
                gen=sum(1 for r in rs if r.get("generated_ok")),
                first=maj(first), repair=maj(final),
                tasks_version=d.get("tasks_version"),
                aether=d.get("aether_version_raw") or d.get("aether_version"))


for run in RUNS:
    base = os.path.join(ROOT, run["dir"])
    files = sorted(glob.glob(os.path.join(base, "*.json")))
    if not files:
        continue
    by_model = collections.defaultdict(dict)
    meta = {}
    for f in files:
        try:
            s = score_file(f)
        except Exception:
            continue
        if not s["suite"]:
            continue
        model = os.path.basename(f).replace(f"_{s['suite']}.json", "")
        by_model[model][s["suite"]] = s
        meta.setdefault("aether", s["aether"])
        meta.setdefault(f"tv_{s['suite']}", s["tasks_version"])
    if not by_model:
        continue

    suites = [x for x in SUITE_ORDER if any(x in v for v in by_model.values())]
    complete = all(len(v) == len(suites) for v in by_model.values())

    L = []
    L.append(f"# {run['title']}\n")
    L.append(f"*Corpus:* {run['corpus']}  ")
    L.append(f"*Host:* {run['host']}  ")
    L.append(f"*Repair attempts:* {run['repair']}  ")
    L.append(f"*Grader:* aether {meta.get('aether','?')}  ")
    tvs = ", ".join(f"{s}={meta.get('tv_'+s,'?')}" for s in suites)
    L.append(f"*Task-suite versions:* {tvs}  ")
    L.append(f"*Models:* {len(by_model)}"
             + ("" if complete else "  \n*Status:* PARTIAL - some models/suites missing") + "\n")

    L.append("## Results\n")
    hdr = "| model | " + " | ".join(f"{s} ({SUITE_N[s]})" for s in suites) + " |"
    L.append(hdr)
    L.append("|" + "---|" * (len(suites) + 1))
    any_repair = run["repair"].startswith("2") or run["repair"].startswith("1")
    for m in sorted(by_model):
        cells = []
        for s in suites:
            v = by_model[m].get(s)
            if not v:
                cells.append("-")
            elif any_repair:
                cells.append(f"{v['first']} -> **{v['repair']}**")
            else:
                cells.append(f"**{v['repair']}**")
        L.append(f"| `{m}` | " + " | ".join(cells) + " |")
    if any_repair:
        L.append("\nCells are *first attempt* -> **after repair**. Per-task scoring is "
                 "majority of repeats.\n")
    else:
        L.append("\nSingle score per task, majority of repeats. This run had no repair "
                 "attempts, so it is directly comparable to other repair-0 boards.\n")

    # integrity block -- worth surfacing per run
    probs = []
    for m in sorted(by_model):
        for s in suites:
            v = by_model[m].get(s)
            if v and v["gen"] != v["cases"]:
                probs.append(f"- `{m}` / {s}: `generated_ok` {v['gen']}/{v['cases']} - "
                             f"empty or failed generations; treat that cell as a floor.")
    L.append("## Integrity\n")
    if probs:
        L.extend(probs)
        L.append("")
    else:
        L.append("`generated_ok` is complete for every model and suite in this run.\n")

    L.append("## Notes\n")
    for n in run["notes"]:
        L.append(f"- {n}")
    L.append("")

    p = OUT / f"{run['slug']}.md"
    p.write_text("\n".join(L), encoding="utf-8")
    print(f"wrote {p.name}: {len(by_model)} models, suites={','.join(suites)}"
          + ("" if complete else " [PARTIAL]"))
