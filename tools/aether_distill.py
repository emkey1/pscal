#!/usr/bin/env python3
"""Distil the guide into the model: verified task -> Aether pairs from a guided model.

The untrained base model writes correct Aether far more often with the guide in
its prompt than a fine-tune does without one. This pipeline harvests that: fresh
tasks, solved twice by independent models, kept only where both agree.

    python3 tools/aether_distill.py BATCH tasks --count 300      # open lane (T'Ra `large-open`) writes task statements
    python3 tools/aether_distill.py BATCH python-cmd               # harness command: MiMo writes Python, output hidden
    python3 tools/aether_distill.py BATCH freeze                   # Python stdout becomes each task's expected stdout
    python3 tools/aether_distill.py BATCH provenance               # every generation request served by an allowed model?
    python3 tools/aether_distill.py BATCH aether-cmd               # harness command: guided base model writes Aether
    python3 tools/aether_distill.py BATCH harvest                  # exact Aether programs -> instruction pairs

BATCH is a directory under Tests/aether_specialization/distill/. Every stage
reads and writes files there and can be re-run; `tasks` appends until --count.

Correctness: a pair is kept only when the guided model's Aether prints exactly
what an independently written Python program (another model) prints for the same
task statement. Pairs are rendered in the board's own prompt format with no guide
(`build_prompt("none", ...)`), so the fine-tune trains on the shape it is scored
on. Tasks avoid every board and sealed topic, and harvested pairs go through
tools/aether_specialization_check_pairs.py (stdout and similarity gates) before
they can train.

The python-cmd / aether-cmd stages print the harness command rather than running
it: the Python stage should run where model-written Python may execute (a claw),
the Aether stage where the guided model is served.

Provenance: task statements come only from the proxy's open lane (T'Ra model
`large-open`, target m4_proxy_large_open, proxy route default-large-open), whose
pool holds only members marked "training_use": "allowed" (MiMo-V2.6-Flash, MIT;
Bonsai 2 27B, Apache-2.0); it answers 503 rather than fall back to a restricted
or unknown model. Each tasks.json request records the T'Ra job, model and the
target that served it. The proxy's X-Training-Use header does not reach this
tool through T'Ra, so per-request member provenance lives on m4 in
~/.openclaw/workspace/dashboard/state/proxy_requests.jsonl: records with
srv=default-large-open carry open_only, training_use, member and model. The
provenance stage reads them.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import random
import re
import sys
import time

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_doc_bench as adb  # noqa: E402
import aether_specialization_corpus_policy as policy  # noqa: E402
import fleet_env  # noqa: E402

REPO_ROOT = _SCRIPT_DIR.parent
DISTILL_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "distill"

CATEGORIES = [
    "table-based dynamic programming (counting ways, best totals, reachability) with small fixed inputs",
    "grid simulations on a small fixed 2-D grid (cellular rules, paths, regions, scoring)",
    "number theory and integer arithmetic (divisors, digits, bases, modular patterns)",
    "text processing on short fixed strings (counting, transforming, validating, formatting)",
    "records and aggregation over a fixed list of items (totals, ranking, grouping by a key, filtering)",
    "stack, queue and ring-buffer simulations driven by a fixed list of operations",
    "parsing simple line-based text formats given inline (key=value lines, commands, fixed-width fields)",
    "scheduling and simulation with a fixed timeline of events (queues, timers, resource use)",
    "small combinatorics and recursion (generating or counting arrangements of a few items)",
    "matrix operations on small fixed matrices",
    "finite state machines processing a fixed input sequence",
    "searching and scanning arrays (pairs, windows of fixed size, longest runs, first/last occurrences)",
    "sorting and selection written by hand (insertion, selection, counting sort; ranking with ties)",
    "date-free calendar and time arithmetic in minutes and seconds (no real dates, no leap years)",
]
DOMAINS = ["a bakery", "a library", "a train network", "a garden", "a warehouse", "a chess club", "a weather station",
           "a music playlist", "a bike-share", "a school timetable", "a parking garage", "a board game", "an orchard",
           "a ferry service", "a recipe book", "a fitness log", "a museum", "a vending machine", "a robot on a grid",
           "a lighthouse", "a beekeeper", "a marathon", "a print shop", "an aquarium", "a mountain hut"]
AVOID = ("factorial; Euclidean GCD alone; FizzBuzz; Fibonacci; Sieve of Eratosthenes; Collatz; Towers of Hanoi; "
         "bubble sort; binary search; N-Queens; coin change; longest increasing subsequence; longest common "
         "subsequence; edit distance; merge sort; quicksort; BFS traversal; Dijkstra; substring search; spiral fill; "
         "expression evaluator; run-length encoding; heaps; topological sort; interval merging; LRU cache; "
         "union-find; sliding window maximum; bracket validation; knapsack; big-number addition; merging k sorted "
         "lists; league tables; tokenizer positions; ledger rounding; Luhn; histogram bins; duration phrases; leap "
         "years; word wrap; semantic versions; CSV quoting; Roman numerals; percentiles; JSON or TOON parsing")

TASK_PROMPT = """You write programming exercises for a benchmark of a small, simple, statically typed imperative \
language. It has functions, loops, if/else, Int, Real, Bool and Text values, arrays and 2-D arrays, records with \
fields, and basic string functions. It has no hash maps, no regular expressions, no built-in sort, and no \
exceptions.

Write {k} new exercises. Category: {category}. Set each one in a different everyday setting; suggestions: {domains}.

Rules for every exercise:
- Self-contained: all input data is written literally in the statement (at most about 20 values or a short text). \
No files, no user input, no randomness, no clocks, no network.
- Exactly one correct output. Specify the EXACT output format line by line: labels, separators, spacing, order. \
If a Real is printed, say how many decimal places, and choose data so no value falls exactly halfway when rounded.
- Between 2 and 15 output lines. No trailing spaces. Do not state the expected output and do not hint at an \
implementation.
- Solvable with loops, arrays and helper functions in about 20-60 lines.
- Avoid these topics entirely: {avoid}.
- Avoid these titles, already used: {used}.

Return only a JSON array of objects with keys "title" (short, unique) and "prompt" (the full statement). \
No commentary, no Markdown fences."""


def batch_dir(name: str) -> pathlib.Path:
    path = DISTILL_DIR / name
    path.mkdir(parents=True, exist_ok=True)
    return path


def load_json(path: pathlib.Path, default):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else default


def save_json(path: pathlib.Path, data) -> None:
    path.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def word_shingles(text: str, n: int = 5) -> set:
    words = re.findall(r"[a-z0-9]+", text.lower())
    return {tuple(words[i:i + n]) for i in range(len(words) - n + 1)}


def jaccard(a: set, b: set) -> float:
    return len(a & b) / len(a | b) if a and b else 0.0


def board_prompts() -> list[tuple[str, set]]:
    out = []
    for manifest in policy.default_board_manifests():
        for task in policy.load_tasks(manifest):
            out.append((f"{manifest.stem}:{task.get('id')}", word_shingles(task.get("prompt") or "")))
    return out


def parse_task_array(text: str) -> list[dict]:
    text = adb.strip_markdown_fences(text or "").strip()
    start, end = text.find("["), text.rfind("]")
    if start == -1 or end <= start:
        return []
    try:
        items = json.loads(text[start:end + 1])
    except json.JSONDecodeError:
        return []
    return [i for i in items if isinstance(i, dict) and isinstance(i.get("title"), str) and isinstance(i.get("prompt"), str)]


def tra_destination(model: str, target: str, temperature: float, max_tokens: int) -> adb.Destination:
    return adb.Destination(destination_id=f"distill-{model}", kind="tra_queue", model=model,
                           base_url=fleet_env.get("TRA_QUEUE_URL"), temperature=temperature,
                           max_output_tokens=max_tokens, request_timeout_seconds=1800, preferred_targets=[target],
                           extra_body={"disable_thinking": True, "submitter": "aether-distill"})


def stage_tasks(args) -> int:
    out = batch_dir(args.batch)
    tasks_path = out / "tasks.json"
    doc = load_json(tasks_path, {"version": args.batch, "description": "distillation tasks (generated)", "tasks": []})
    tasks = doc["tasks"]
    boards = board_prompts()
    rng = random.Random(len(tasks) + 7)
    dest = tra_destination(args.model, args.target, 0.8, 8000)
    log = open(out / "tasks.log", "a", encoding="utf-8")
    rejected = {"duplicate": 0, "board": 0, "short": 0}
    while len(tasks) < args.count:
        category = CATEGORIES[len(tasks) // args.per_request % len(CATEGORIES)]
        used = "; ".join(t["title"] for t in tasks[-60:]) or "none yet"
        prompt = TASK_PROMPT.format(k=args.per_request, category=category, domains=", ".join(rng.sample(DOMAINS, 5)),
                                    avoid=AVOID, used=used)
        started = time.time()
        try:
            reply = adb.invoke_tra_queue(prompt, dest)
        except Exception as exc:  # noqa: BLE001 - a failed request is logged and retried
            log.write(f"request failed: {exc}\n")
            time.sleep(10)
            continue
        doc.setdefault("requests", []).append({"t0": round(started, 1), "t1": round(time.time(), 1),
                                               "model": args.model, "target": reply.get("model"),
                                               "job": reply.get("response_id")})
        items = parse_task_array(reply.get("raw_text", ""))
        kept = 0
        for item in items:
            shingles = word_shingles(item["prompt"])
            if len(item["prompt"]) < 80:
                rejected["short"] += 1
                continue
            if any(jaccard(shingles, word_shingles(t["prompt"])) >= 0.5 or t["title"].lower() == item["title"].lower()
                   for t in tasks):
                rejected["duplicate"] += 1
                continue
            if any(jaccard(shingles, ref) >= 0.35 for _, ref in boards):
                rejected["board"] += 1
                continue
            tasks.append({"id": f"dst_{args.batch.replace('-', '_')}_{len(tasks):04d}", "title": item["title"].strip(),
                          "prompt": item["prompt"].strip(), "expected_stdout": "", "hide_expected_stdout": True,
                          "reference_solution": "", "category": category, "achievability": "distill-candidate"})
            kept += 1
        save_json(tasks_path, doc)
        log.write(f"{time.strftime('%H:%M:%S')} {category[:40]!r}: {len(items)} returned, {kept} kept, "
                  f"{len(tasks)} total, {time.time() - started:.0f}s, rejected so far {rejected}\n")
        log.flush()
        print(f"{len(tasks)}/{args.count} tasks ({kept} kept of {len(items)}; rejected {rejected})", flush=True)
    return 0


# Runs on the proxy host: which pool members served requests inside the windows,
# mapped to the model each member serves (from the routes file).
PROVENANCE_PROBE = r"""
import json, os, sys
windows, pool, log_path, routes_path = json.loads(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
models = {}
def walk(o):
    if isinstance(o, dict):
        up = o.get("upstream") or o.get("_parked_upstream")
        if isinstance(up, str) and "default_model" in o:
            models[up.split("://", 1)[-1].rstrip("/")] = o["default_model"]
        for v in o.values():
            walk(v)
    elif isinstance(o, list):
        for v in o:
            walk(v)
walk(json.load(open(os.path.expanduser(routes_path))))
counts, uses = {}, {}
for line in open(os.path.expanduser(log_path)):
    try:
        r = json.loads(line)
    except ValueError:
        continue
    if r.get("pool") != pool or not any(a <= r.get("t", 0) <= b for a, b in windows):
        continue
    member = str(r.get("member"))
    key = r.get("model") or models.get(member) or models.get(member.split("/", 1)[0]) or ("unknown member " + member)
    counts[key] = counts.get(key, 0) + 1
    use = r.get("training_use", "not logged")
    uses[use] = uses.get(use, 0) + 1
print(json.dumps({"counts": counts, "training_use": uses}))
"""


def stage_provenance(args) -> int:
    """Refuse a batch any of whose generation requests a restricted model may have served.

    The proxy log does not say which client sent a request, so every request to the
    pool inside the batch's windows counts: one served by a model outside --allow
    fails the batch, whoever sent it.
    """
    out = batch_dir(args.batch)
    windows = []
    if args.since and args.until:
        windows.append((args.since, args.until))
    else:
        for r in load_json(out / "tasks.json", {}).get("requests", []):
            windows.append((r["t0"] - 5, r["t1"] + 5))
        report = load_json(out / "python.json", None)
        if report:
            windows.append((report["created_at_unix"] - 5, report.get("updated_at_unix", time.time()) + 5))
    if not windows:
        raise SystemExit("no request windows recorded; pass --since/--until (epoch seconds)")
    import subprocess
    import shlex
    remote = " ".join(shlex.quote(a) for a in ["python3", "-", json.dumps(windows), args.pool, args.proxy_log,
                                                args.proxy_routes])
    probe = subprocess.run(["ssh", "-o", "BatchMode=yes", args.proxy_host, remote],
                           input=PROVENANCE_PROBE, text=True, capture_output=True, timeout=300)
    if probe.returncode != 0:
        raise SystemExit(f"provenance probe failed: {probe.stderr.strip()[:300]}")
    found = json.loads(probe.stdout)
    counts, uses = found["counts"], found["training_use"]
    allowed = set(args.allow)
    bad = {model: n for model, n in counts.items() if model not in allowed}
    # Records from before the proxy logged training_use (2026-10-17) say "not logged".
    bad_use = {use: n for use, n in uses.items() if use not in ("allowed", "not logged")}
    verdict = {"ok": not bad and not bad_use and bool(counts), "pool": args.pool, "allowed_models": sorted(allowed),
               "windows": windows, "requests_by_model": counts, "requests_by_training_use": uses,
               "disallowed": bad, "disallowed_training_use": bad_use,
               "checked_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    save_json(out / "provenance.json", verdict)
    print(json.dumps(verdict, indent=1))
    return 0 if verdict["ok"] else 1


def harness_cmd(args, *, tasks: str, report: str, extra: list[str]) -> str:
    return " ".join(["python3 -u tools/aether_doc_bench.py", "--destinations-config", args.destinations,
                     "--tasks", tasks, "--repeats 1", "--seed-base 42", "--aether-bin", args.aether_bin,
                     "--allow-skew --keep-loaded --resume", "--output-json", report, *extra, "--text-summary"])


def stage_python_cmd(args) -> int:
    out = batch_dir(args.batch)
    print(harness_cmd(args, tasks=str(out / "tasks.json"), report=str(out / "python.json"),
                      extra=["--docs none --python-baseline --skip-aether --repair-attempts 0 --allow-broken-oracle"]))
    return 0


def stage_freeze(args) -> int:
    out = batch_dir(args.batch)
    tasks = {t["id"]: t for t in load_json(out / "tasks.json", {"tasks": []})["tasks"]}
    report = load_json(out / "python.json", None)
    if report is None:
        raise SystemExit("no python.json yet: run the python-cmd command first")
    board_stdout = {t.get("expected_stdout") for m in policy.default_board_manifests() for t in policy.load_tasks(m)}
    frozen, reasons = [], {}
    for result in report["destinations"][0]["variants"][0].get("python_baseline_results", []):
        attempt = (result.get("attempts") or [result])[0]
        run = attempt.get("run") or result.get("run") or {}
        stdout = run.get("stdout") or ""
        task = tasks.get(result.get("task_id"))
        why = None
        if task is None:
            why = "unknown task"
        elif run.get("returncode") != 0:
            why = "python failed"
        elif not stdout.strip():
            why = "no output"
        elif stdout.count("\n") > 30 or len(stdout) > 1500:
            why = "output too long"
        elif "Traceback" in stdout or stdout in board_stdout:
            why = "unusable output"
        if why:
            reasons[why] = reasons.get(why, 0) + 1
            continue
        frozen.append(task | {"expected_stdout": stdout, "hide_expected_stdout": False,
                              "achievability": "python-agreed-candidate"})
    save_json(out / "tasks_frozen.json", {"version": args.batch, "description": "distillation tasks with the "
                                          "independent Python program's stdout as expected output", "tasks": frozen})
    print(f"frozen {len(frozen)} of {len(tasks)} tasks; dropped {reasons}")
    return 0


def stage_aether_cmd(args) -> int:
    out = batch_dir(args.batch)
    print(harness_cmd(args, tasks=str(out / "tasks_frozen.json"), report=str(out / "aether.json"),
                      extra=[f"--docs {args.docs} --repair-attempts 2"]))
    return 0


def stage_harvest(args) -> int:
    out = batch_dir(args.batch)
    frozen = {t["id"]: t for t in load_json(out / "tasks_frozen.json", {"tasks": []})["tasks"]}
    report = load_json(out / "aether.json", None)
    if report is None:
        raise SystemExit("no aether.json yet: run the aether-cmd command first")
    provenance = load_json(out / "provenance.json", None)
    if not args.skip_provenance and not (provenance and provenance.get("ok")):
        raise SystemExit("no passing provenance.json: run the provenance stage (task and Python generation must "
                         "come only from models whose outputs may train), or pass --skip-provenance")
    pairs, first, repaired = [], 0, 0
    for result in report["destinations"][0]["variants"][0]["results"]:
        task = frozen.get(result.get("task_id"))
        attempts = result.get("attempts") or []
        exact = [a for a in attempts if (a.get("run") or {}).get("exact_stdout_match")]
        if task is None or not exact:
            continue
        source = exact[0].get("source_code") or ""
        if attempts[0] is exact[0]:
            first += 1
        else:
            repaired += 1
        board_task = adb.Task(task_id=task["id"], title=task["title"], prompt=task["prompt"],
                              expected_stdout=task["expected_stdout"])
        pairs.append({"id": task["id"], "prompt": adb.build_prompt("none", "", board_task), "solution": source,
                      "expected_stdout": task["expected_stdout"],
                      "_provenance": {"category": task.get("category"), "first_attempt": attempts[0] is exact[0],
                                      "solver": report["destinations"][0].get("model"),
                                      "guide": report["destinations"][0]["variants"][0].get("doc_version"),
                                      "grader": report.get("aether_version"),
                                      "generator_models": sorted((provenance or {}).get("requests_by_model", {})),
                                      "generator_pool": (provenance or {}).get("pool")}})
    save_json(out / "pairs.json", {"pairs": pairs})
    print(f"harvested {len(pairs)} pairs ({first} first-attempt, {repaired} after repair) of {len(frozen)} frozen "
          f"tasks; next: tools/aether_specialization_check_pairs.py {out / 'pairs.json'}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("batch")
    ap.add_argument("stage", choices=["tasks", "python-cmd", "freeze", "provenance", "aether-cmd", "harvest"])
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--per-request", type=int, default=8)
    # Change these two together. T'Ra serves `large-open` only on m4_proxy_large_open, so with that
    # target missing or disabled the job waits; a `large` job would fall back to m4_proxy_large and
    # so to restricted models (Claude Haiku: its outputs may not train) even with this target preferred.
    ap.add_argument("--model", default="large-open", help="T'Ra model for task writing (open lane only)")
    ap.add_argument("--target", default="m4_proxy_large_open", help="T'Ra target for task writing")
    ap.add_argument("--destinations", default="DESTINATIONS.json", help="harness destinations file (cmd stages)")
    ap.add_argument("--aether-bin", default="build/bin/aether")
    ap.add_argument("--docs", default="medium")
    ap.add_argument("--allow", action="append", default=None,
                    help="model whose outputs may be training data (repeatable; default mimo-v2.6-flash, MIT, "
                         "and bonsai-2-27b, Apache-2.0)")
    ap.add_argument("--pool", default="large-open", help="proxy pool to check (batches before 2026-10-17: large)")
    ap.add_argument("--proxy-host", default="m4")
    ap.add_argument("--proxy-log", default="~/.openclaw/workspace/dashboard/state/proxy_requests.jsonl")
    ap.add_argument("--proxy-routes", default="~/.openclaw/workspace/dashboard/ai-routes.json")
    ap.add_argument("--since", type=float, default=None, help="provenance window start (epoch s), with --until")
    ap.add_argument("--until", type=float, default=None)
    ap.add_argument("--skip-provenance", action="store_true", help="harvest without a passing provenance check")
    args = ap.parse_args()
    args.allow = args.allow or ["mimo-v2.6-flash", "bonsai-2-27b"]
    return {"provenance": stage_provenance, "tasks": stage_tasks, "python-cmd": stage_python_cmd, "freeze": stage_freeze,
            "aether-cmd": stage_aether_cmd, "harvest": stage_harvest}[args.stage](args)


if __name__ == "__main__":
    sys.exit(main())
