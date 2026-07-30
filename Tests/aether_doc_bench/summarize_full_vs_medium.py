#!/usr/bin/env python3
"""Aggregate the full-guide vs medium-guide board into comparison tables.

Reads every <model>_<suite>.json report in a results directory and prints:
  1. a per-model, per-suite exact-match table with the full->medium delta
  2. a per-model roll-up across all suites
  3. token accounting, including how much the provider served from cache
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

SUITES = ["simple", "large", "cs", "nontoon"]
VARIANTS = ["full", "medium"]


def load_reports(outdir: pathlib.Path) -> dict[tuple[str, str], dict]:
    reports: dict[tuple[str, str], dict] = {}
    for path in sorted(outdir.glob("*.json")):
        stem = path.stem
        for suite in SUITES:
            if stem.endswith("_" + suite):
                model = stem[: -(len(suite) + 1)]
                try:
                    reports[(model, suite)] = json.loads(path.read_text())
                except json.JSONDecodeError:
                    print(f"warn: {path.name} is not complete JSON (run still in flight?)", file=sys.stderr)
                break
    return reports


def variant_of(report: dict, doc_name: str) -> dict | None:
    for dest in report.get("destinations", []):
        for variant in dest.get("variants", []):
            if variant.get("doc_name") == doc_name:
                return variant
    return None


def cell(variant: dict | None) -> tuple[int, int, int, int]:
    """(exact, run_ok, total, resolved_after_repair)"""
    if not variant:
        return (0, 0, 0, 0)
    s = variant.get("summary", {})
    return (
        s.get("exact_stdout_match", 0),
        s.get("run_ok", 0),
        s.get("total_cases", 0),
        s.get("resolved_after_repair", 0),
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("outdir", type=pathlib.Path)
    args = ap.parse_args()

    reports = load_reports(args.outdir)
    if not reports:
        print(f"no reports under {args.outdir}", file=sys.stderr)
        return 1

    models = sorted({m for m, _ in reports})

    print(f"# Full vs medium guide board — {args.outdir.name}\n")
    any_report = next(iter(reports.values()))
    print(f"aether: {any_report.get('aether_version')}")
    for suite in SUITES:
        for model in models:
            r = reports.get((model, suite))
            if r:
                print(f"suite {suite:8} manifest {pathlib.Path(r['tasks_file']).name} v{r.get('tasks_version')}")
                break
    for name in VARIANTS:
        for r in reports.values():
            v = variant_of(r, name)
            if v:
                print(f"guide {name:7} {v['doc_bytes']:>7} bytes  ~{v['doc_approx_tokens']} tok")
                break
    print()

    # --- per-suite exact-match table ---
    print("## Exact stdout match (exact/total), full -> medium\n")
    header = f"| {'model':24} |"
    for suite in SUITES:
        header += f" {suite:^17} |"
    header += "  total full  | total med | delta |"
    print(header)
    print("|" + "-" * 26 + "|" + ("-" * 19 + "|") * len(SUITES) + "-" * 14 + "|" + "-" * 11 + "|" + "-" * 7 + "|")

    roll: dict[str, dict[str, list[int]]] = {}
    for model in models:
        row = f"| {model:24} |"
        tf = tm = nf = nm = 0
        incomplete = False
        for suite in SUITES:
            r = reports.get((model, suite))
            fe, _, fn, _ = cell(variant_of(r, "full")) if r else (0, 0, 0, 0)
            me, _, mn, _ = cell(variant_of(r, "medium")) if r else (0, 0, 0, 0)
            # Only compare a suite whose two variants both ran to the same case
            # count. A finished `full` scored against an in-flight `medium`
            # produces a large fake delta, which is worse than no number.
            if not r or fn == 0 or mn == 0 or fn != mn:
                row += f" {'(pending)' if r else '-':^17} |"
                incomplete = True
                continue
            row += f" {f'{fe}/{fn} -> {me}/{mn}':^17} |"
            tf += fe
            tm += me
            nf += fn
            nm += mn
        if incomplete:
            row += f"  {tf}/{nf:<8} | {tm}/{nm:<6} | partial|"
        else:
            row += f"  {tf}/{nf:<8} | {tm}/{nm:<6} | {tm - tf:+d}   |"
        roll[model] = {"full": [tf, nf], "medium": [tm, nm]}
        print(row)

    # --- token accounting ---
    print("\n## Prompt-token cost and provider-side cache hits\n")
    print(f"| {'model':24} | {'variant':7} | {'prompt tok':>11} | {'cached tok':>11} | {'cache %':>7} | {'completion':>10} |")
    print("|" + "-" * 26 + "|" + "-" * 9 + "|" + "-" * 13 + "|" + "-" * 13 + "|" + "-" * 9 + "|" + "-" * 12 + "|")
    totals = {v: [0, 0, 0] for v in VARIANTS}
    for model in models:
        for name in VARIANTS:
            p = c = comp = 0
            for suite in SUITES:
                r = reports.get((model, suite))
                if not r:
                    continue
                v = variant_of(r, name)
                if not v:
                    continue
                u = v.get("usage_summary", {})
                p += u.get("prompt_tokens_total") or 0
                c += u.get("cached_tokens_total") or 0
                comp += u.get("completion_tokens_total") or 0
            if not p:
                continue
            totals[name][0] += p
            totals[name][1] += c
            totals[name][2] += comp
            print(f"| {model:24} | {name:7} | {p:>11,} | {c:>11,} | {100*c/p:>6.1f}% | {comp:>10,} |")
    print("|" + "-" * 26 + "|" + "-" * 9 + "|" + "-" * 13 + "|" + "-" * 13 + "|" + "-" * 9 + "|" + "-" * 12 + "|")
    for name in VARIANTS:
        p, c, comp = totals[name]
        if p:
            print(f"| {'ALL':24} | {name:7} | {p:>11,} | {c:>11,} | {100*c/p:>6.1f}% | {comp:>10,} |")
    if totals["full"][0] and totals["medium"][0]:
        saved = totals["full"][0] - totals["medium"][0]
        print(f"\nmedium sent {saved:,} fewer prompt tokens than full "
              f"({100*saved/totals['full'][0]:.1f}% less) for the same 73 tasks.")

    # --- tasks that flipped ---
    print("\n## Tasks whose exact-match verdict differs between guides\n")
    for model in models:
        flips: list[str] = []
        for suite in SUITES:
            r = reports.get((model, suite))
            if not r:
                continue
            fv, mv = variant_of(r, "full"), variant_of(r, "medium")
            if not fv or not mv:
                continue
            fmap = {x["task_id"]: x for x in fv.get("results", [])}
            mmap = {x["task_id"]: x for x in mv.get("results", [])}
            for tid in sorted(set(fmap) & set(mmap)):
                fok = bool(fmap[tid].get("run", {}).get("exact_stdout_match"))
                mok = bool(mmap[tid].get("run", {}).get("exact_stdout_match"))
                if fok != mok:
                    flips.append(f"    {suite:8} {tid:28} full={'PASS' if fok else 'FAIL'} medium={'PASS' if mok else 'FAIL'}")
        if flips:
            print(f"  {model}")
            print("\n".join(flips))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
