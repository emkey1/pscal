#!/usr/bin/env python3
"""Fail on a report whose generations are empty or reasoning traces (written by aether_train_run.py).

    check_generations.py REPORT.json LEAK_PREFIXES_JSON [--complete]

--complete also fails when a case was never measured (infra failure or missing).
Prints the first-attempt and final exact counts either way.
"""
import json, sys

path, prefixes = sys.argv[1], [p.lower() for p in json.loads(sys.argv[2])]
try:
    report = json.load(open(path))
except (OSError, ValueError) as exc:
    print(f"check: cannot read {path}: {exc}")
    sys.exit(1)
variant = report["destinations"][0]["variants"][0]
results, summary = variant["results"], variant["summary"]
def first_source(r):
    attempts = r.get("attempts") or []
    return ((attempts[0].get("source_code") if attempts else None) or r.get("source_code") or "")
empty = sum(1 for r in results if not first_source(r).strip())
leak = sum(1 for r in results if prefixes and first_source(r).lstrip().lower().startswith(tuple(prefixes)))
infra = summary.get("infra_failed", 0)
print(f"check {path.rsplit('/', 1)[-1]}: cases={summary['total_cases']} FA={summary.get('first_attempt_exact')} "
      f"FX={summary.get('final_exact')} empty={empty} leak={leak} infra={infra}")
bad = leak > 0 or (results and empty == len(results))
if "--complete" in sys.argv[3:]:
    bad = bad or infra > 0 or report.get("infra_failed")
sys.exit(1 if bad else 0)
