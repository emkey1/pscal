#!/usr/bin/env python3
"""Roll the B0 unit reports up into board summaries and a MANIFEST.

    python3 Tests/aether_doc_bench/results/b0_20261007/summarize_b0.py [--dir RESULTS_DIR] [--weights FILE] [--include-pending]

Reads <board>/<dest>__<suite>__r<R>.json under RESULTS_DIR (default: this
directory; one report per unit, written by run_b0_lane.sh) and writes
summary.json, summary.md and MANIFEST.json there. A report with several guide
variants (B1: medium and card) gives one row set per variant ("B1:medium",
"B1:card"); the Python baseline board (BP) is read from python_baseline_results.
--include-pending also reads <board>/infra_pending/*.json -- units closed with
unmeasured repair attempts -- and marks them in the tables and the MANIFEST. Per-case numbers come from the harness's own
summarize() and classify_attempt(), so every cell matches what --resummarize
prints for the same cases.

Intervals:
  wilson   case-level Wilson 95% (the harness's fa_ci95/fx_ci95). Repeats of a
           task are not independent, so this is a lower bound on uncertainty.
  boot     95% percentile bootstrap, 4000 draws, resampling task clusters within
           each (model, suite): all repeats of a task go in or out together.
           This is the interval to quote for a pooled row.
Between-repeat spread: FA per repeat r (seed 42+r) over the same cells, and its
standard deviation, which plan 2.2 uses to size B_G1's repeats.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import importlib.util
import json
import pathlib
import random
import re
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[3]
UNIT_RE = re.compile(r"^(?P<dest>.+)__(?P<suite>tasks_[a-z0-9_]+)__r(?P<r>\d+)\.json(?:\.gz)?$")
BOARDS = ("B0", "B0-small", "B0-full", "B1", "P0", "S0", "BP")
CLASSES = ("pass", "silent_wrong", "crash_hang", "no_answer", "uncoded_error", "coded_error", "infra_failed", "not_sent")


def load_harness():
    spec = importlib.util.spec_from_file_location("aether_doc_bench", REPO / "tools" / "aether_doc_bench.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["aether_doc_bench"] = mod
    spec.loader.exec_module(mod)
    return mod


def sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pct(x: float | None) -> str:
    return "-" if x is None else f"{100 * x:.1f}"


def ci(pair) -> str:
    return "-" if not pair else f"[{pct(pair[0])}, {pct(pair[1])}]"


def bootstrap(cases: list[dict], key: str, draws: int = 4000, seed: int = 7) -> list[float] | None:
    """Percentile CI of the pooled rate, resampling task clusters per stratum."""
    strata: dict[tuple, dict[str, list[int]]] = {}
    for c in cases:
        strata.setdefault((c["dest"], c["suite"]), {}).setdefault(c["task"], []).append(int(c[key]))
    if not strata:
        return None
    rng = random.Random(seed)
    groups = [list(t.values()) for t in strata.values()]
    stats = []
    for _ in range(draws):
        hit = tot = 0
        for clusters in groups:
            for _ in range(len(clusters)):
                pick = clusters[rng.randrange(len(clusters))]
                hit += sum(pick)
                tot += len(pick)
        stats.append(hit / tot)
    stats.sort()
    return [round(stats[int(0.025 * draws)], 4), round(stats[int(0.975 * draws) - 1], 4)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", help="JSON of panel weight-file sha256s (from hash_weights.py)")
    ap.add_argument("--dir", help="results directory to summarize (default: this script's directory)")
    ap.add_argument("--include-pending", action="store_true", help="also read <board>/infra_pending/*.json")
    ap.add_argument("--board-set", help="name recorded in the MANIFEST (default: the directory name)")
    args = ap.parse_args()
    global HERE
    if args.dir:
        HERE = pathlib.Path(args.dir).resolve()
    bench = load_harness()

    units: list[dict] = []
    cases: list[dict] = []
    for board_dir in BOARDS:
        paths = [(p, False) for p in sorted((HERE / board_dir).glob("*.json*"))]
        if args.include_pending:
            paths += [(p, True) for p in sorted((HERE / board_dir / "infra_pending").glob("*.json*"))]
        for path, pending in paths:
            m = UNIT_RE.match(path.name)
            if not m:
                continue
            # Committed reports are scrubbed and gzipped by results/pack_reports.py.
            report = json.loads(gzip.decompress(path.read_bytes()) if path.suffix == ".gz" else path.read_text())
            dest = report["destinations"][0]
            variants = dest["variants"]
            for variant in variants:
              python = board_dir == "BP"
              results = variant.get("python_baseline_results") if python else variant["results"]
              if not results:
                  continue
              doc = "python" if python else variant.get("doc_name")
              board = board_dir if len(variants) == 1 else f"{board_dir}:{doc}"
              units.append({"board": board, "file": str(path.relative_to(HERE)), "dest": m["dest"], "suite": m["suite"],
                          "repeat": int(m["r"]), "report": report, "doc": doc, "pending": pending,
                          "results": results})
              for case in results:
                  first = bench.classify_attempt(bench.first_attempt_of(case))
                  final = bench.classify_attempt((case.get("attempts") or [case])[-1])
                  cases.append({"board": board, "dest": m["dest"], "suite": m["suite"], "repeat": int(m["r"]),
                                "task": case["task_id"], "seed": case.get("seed"),
                                "fa": first == "pass", "fx": bool(case["run"]["exact_stdout_match"]),
                              # FX as the 2026-10-12 rule scores it: an empty first attempt is final.
                              "fx_nr": bool(case["run"]["exact_stdout_match"]) and first != "no_answer",
                                "first_class": first, "final_class": final,
                                "repaired": bool(case.get("resolved_after_repair")),
                                "attempts": len(case.get("attempts") or [])})

    def rollup(sel: list[dict], results: list[dict]) -> dict:
        s = bench.summarize(results)
        out = {k: s[k] for k in ("total_cases", "first_attempt_exact", "fa_rate", "fa_ci95", "final_exact",
                                 "fx_rate", "fx_ci95", "first_attempt_classes", "infra_failed", "tasks",
                                 "task_majority_fa", "task_majority_fx", "flaky_fa", "flaky_fx")}
        out["fa_boot95"] = bootstrap(sel, "fa")
        out["fx_boot95"] = bootstrap(sel, "fx")
        out["fx_no_retry_rate"] = round(sum(x["fx_nr"] for x in sel) / len(sel), 4) if sel else None
        out["fx_no_retry_boot95"] = bootstrap(sel, "fx_nr")
        out["final_classes"] = {c: sum(1 for x in sel if x["final_class"] == c) for c in CLASSES}
        out["repaired"] = sum(1 for x in sel if x["repaired"])
        by_r: dict[int, list[dict]] = {}
        for x in sel:
            by_r.setdefault(x["repeat"], []).append(x)
        out["fa_by_repeat"] = {str(r): round(sum(x["fa"] for x in v) / len(v), 4) for r, v in sorted(by_r.items())}
        out["fx_by_repeat"] = {str(r): round(sum(x["fx"] for x in v) / len(v), 4) for r, v in sorted(by_r.items())}
        rates = list(out["fa_by_repeat"].values())
        out["fa_repeat_sd"] = round(statistics.stdev(rates), 4) if len(rates) > 1 else None
        return out

    summary: dict = {"boards": {}}
    board_names = sorted({u["board"] for u in units}, key=lambda b: (BOARDS.index(b.split(":")[0]), b))
    for board in board_names:
        bu = [u for u in units if u["board"] == board]
        bc = [c for c in cases if c["board"] == board]
        node = {"units": len(bu), "repeats": sorted({u["repeat"] for u in bu}),
                "pending_units": sorted(u["file"] for u in bu if u["pending"]),
                "pooled": rollup(bc, [r for u in bu for r in u["results"]]), "by_suite": {}, "by_dest": {},
                "cells": {}}
        for suite in sorted({u["suite"] for u in bu}):
            node["by_suite"][suite] = rollup([c for c in bc if c["suite"] == suite],
                                             [r for u in bu if u["suite"] == suite for r in u["results"]])
        for d in sorted({u["dest"] for u in bu}):
            node["by_dest"][d] = rollup([c for c in bc if c["dest"] == d],
                                        [r for u in bu if u["dest"] == d for r in u["results"]])
            for suite in sorted({u["suite"] for u in bu if u["dest"] == d}):
                node["cells"][f"{d}|{suite}"] = rollup(
                    [c for c in bc if c["dest"] == d and c["suite"] == suite],
                    [r for u in bu if u["dest"] == d and u["suite"] == suite for r in u["results"]])
        summary["boards"][board] = node
    (HERE / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")

    # Markdown tables.
    lines = ["# B0 boards: generated summary", "",
             "Generated by `summarize_b0.py` from the unit reports; do not edit by hand.",
             "FA/FX in %. `wilson` = case-level Wilson 95%; `boot` = task-cluster bootstrap 95% (quote this one).",
             "`FX nr` = FX as the rule of 2026-10-12 scores it: a first attempt with no program is final, with no repair",
             "round. These boards did repair such cases; the column makes them comparable with boards run after the rule.", ""]

    def row(label: str, r: dict) -> str:
        fc = {c: r["first_attempt_classes"].get(c, 0) for c in CLASSES}
        return (f"| {label} | {r['total_cases']} | {pct(r['fa_rate'])} | {ci(r['fa_boot95'])} | {ci(r['fa_ci95'])} | "
                f"{pct(r['fx_rate'])} | {ci(r['fx_boot95'])} | {pct(r['fx_no_retry_rate'])} | {fc['silent_wrong']} | {fc['crash_hang']} | "
                f"{fc['no_answer']} | {fc['uncoded_error']} | {fc['coded_error']} | {fc['infra_failed']} | {fc['not_sent']} | "
                f"{r['fa_repeat_sd'] if r['fa_repeat_sd'] is not None else '-'} |")

    head = ("| | cases | FA | FA boot | FA wilson | FX | FX boot | FX nr | SW | crash | no_answer | uncoded | coded | infra | not_sent | FA sd(r) |\n"
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for board, node in summary["boards"].items():
        lines += [f"## {board}", "", f"{node['units']} units, repeats {node['repeats']}. "
                  "Failure-class columns count first attempts.", ""]
        if node["pending_units"]:
            lines += ["Includes unit(s) closed with unmeasured repair attempts (first attempts complete, FX a lower "
                      "bound): " + ", ".join(f"`{f}`" for f in node["pending_units"]), ""]
        lines += [head, row("**pooled**", node["pooled"])]
        lines += [row(f"suite {s}", r) for s, r in node["by_suite"].items()]
        lines += [row(f"model {d}", r) for d, r in node["by_dest"].items()]
        lines += ["", "<details><summary>per model x suite</summary>", "", head]
        lines += [row(k.replace("|", " / "), r) for k, r in node["cells"].items()]
        lines += ["", "</details>", ""]
    (HERE / "summary.md").write_text("\n".join(lines) + "\n")

    # MANIFEST: everything that pins the instrument, plus every unit's provenance.
    def one(values: set):
        return sorted(values)[0] if len(values) == 1 else sorted(values)
    manifest = {
        "board_set": args.board_set or HERE.name,
        "plan_item": "W1-19",
        "binary_sha256": one({u["report"]["binary_sha256"] for u in units}),
        "aether_version": one({u["report"]["aether_version"] for u in units}),
        "build_type": one({u["report"]["toolchain"].get("build_type") for u in units}),
        "aether_checkout": one({json.dumps(u["report"]["provenance"]["aether_root_checkout"], sort_keys=True)
                                for u in units}),
        "harness_sha256": one({u["report"]["harness"]["sha256"] for u in units}),
        "prompt_template": one({json.dumps(u["report"]["harness"].get("prompt_template"), sort_keys=True)
                                for u in units}),
        "umbrella_heads": sorted({u["report"]["provenance"]["umbrella_head"] for u in units}),
        "umbrella_dirty": sorted({bool(u["report"]["provenance"]["umbrella_dirty"]) for u in units}),
        "guides": {},
        "tasks": {},
        "destinations_sha256": sorted({u["report"]["destinations_sha256"] for u in units}),
        "skew_guard": one({json.dumps(u["report"]["skew_guard"], sort_keys=True) for u in units}),
        "units": [],
    }
    for u in units:
        r = u["report"]
        for name, g in (r.get("guides") or {}).items():
            if name == u["doc"] or (u["doc"] == "python" and name == "none"):
                manifest["guides"][name] = {k: g.get(k) for k in ("path", "version", "sha256", "bytes",
                                                                  "approx_tokens", "tokens_o200k")}
        manifest["tasks"][u["suite"]] = {"sha256": r["tasks_sha256"], "version": r.get("tasks_version")}
        rc = r["run_config"]
        manifest["units"].append({
            "file": u["file"], "board": u["board"], "destination": u["dest"], "suite": u["suite"],
            "repeat": u["repeat"], "seed_base": rc.get("seed_base"), "repair_attempts": rc.get("repair_attempts"),
            "doc": u["doc"], "model": r["destinations"][0].get("model"), "pending_infra": u["pending"],
            "cases": len(u["results"]), "file_sha256": sha256_file(HERE / u["file"]),
            "harness_sha256": r["harness"]["sha256"],
        })
    if args.weights:
        manifest["panel_weights"] = json.loads(pathlib.Path(args.weights).read_text())
    for key in ("aether_checkout", "skew_guard", "prompt_template"):
        if isinstance(manifest[key], str):
            manifest[key] = json.loads(manifest[key])
    (HERE / "MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"{len(units)} units, {len(cases)} cases -> summary.json, summary.md, MANIFEST.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
