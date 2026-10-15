#!/usr/bin/env python3
"""Build a small curated OpenClaw SFT dataset for Mike/T'Ra workflows.

The goal is not bulk imitation. It creates reviewable, high-signal examples from
local artifacts and hand-authored policy/decision patterns.
"""
from __future__ import annotations
import argparse, json, re, textwrap
from pathlib import Path
from random import Random
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
SYSTEM = "You are T'Ra, Mike's local OpenClaw assistant. Be concise, careful with private data, local-first, source-aware, and direct about uncertainty. Never invent facts or numbers."


def clean(s: str, limit: int = 6000) -> str:
    s = re.sub(r"\s+", " ", s or "").strip()
    return s[:limit].rstrip() + ("…" if len(s) > limit else "")


def read(path: Path, limit: int = 6000) -> str:
    try:
        return clean(path.read_text(errors="replace"), limit)
    except Exception:
        return ""


def example(category: str, user: str, assistant: str, source: str, quality: str = "curated") -> dict[str, Any]:
    return {
        "category": category,
        "source": source,
        "quality": quality,
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": user.strip()},
            {"role": "assistant", "content": assistant.strip()},
        ],
    }


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(errors="replace"))
    except Exception:
        return None


def finance_examples() -> list[dict[str, Any]]:
    out = []
    issue_dirs = sorted((ROOT / "reports/finance-newsletter").glob("*"))
    for d in issue_dirs:
        if not d.is_dir():
            continue
        summary = read(d / "summary.txt", 5000)
        manifest = load_json(d / "manifest.json") or {}
        newsletter = read(d / "newsletter.md", 7000)
        if summary:
            out.append(example(
                "finance_summary",
                f"Create a concise production summary for this finance newsletter issue. Flag missing or weak data clearly.\n\nIssue material:\n{newsletter[:4500]}",
                summary,
                str(d / "summary.txt"),
            ))
        rows = manifest.get("market_rows") or []
        failures = manifest.get("retrieval_failures") or []
        if rows:
            table = "| Asset | Start | Latest | Week | MTD | YTD | Source |\n|---|---:|---:|---:|---:|---:|---|\n"
            for r in rows[:8]:
                table += f"| {r.get('asset')} | {r.get('start')} | {r.get('end')} | {r.get('weekly_change_pct')}% | {r.get('mtd_change_pct')}% | {r.get('ytd_change_pct')}% | {r.get('source')} |\n"
            disclosure = ""
            if failures:
                disclosure = "\nData caveat: External market-data fetches failed, so local cache values were used and must be disclosed."
            out.append(example(
                "finance_table_rendering",
                "Convert these market rows into a clean report-ready Markdown table and include any necessary data caveat.\n\n" + json.dumps(rows[:8], ensure_ascii=False, indent=2) + ("\nFailures:\n" + "\n".join(failures[:5]) if failures else ""),
                table.strip() + disclosure,
                str(d / "manifest.json"),
            ))
        takeaways = manifest.get("takeaways") or manifest.get("top_takeaways") or []
        if not takeaways and summary:
            lines = [ln for ln in summary.splitlines() if re.match(r"\d+\. ", ln.strip())]
            takeaways = [re.sub(r"^\d+\.\s*", "", ln.strip()) for ln in lines[:5]]
        if takeaways:
            out.append(example(
                "signal_vs_noise",
                "Separate these finance takeaways into signal, noise, and watch-next items. Be skeptical and do not add new numbers.\n\n" + "\n".join(f"- {x}" for x in takeaways[:8]),
                "Signal:\n" + "\n".join(f"- {x}" for x in takeaways[:3]) + "\n\nNoise / weak assumptions:\n- Treat price moves without source freshness, breadth, and rate context as incomplete evidence.\n- Do not infer causality unless the source material supports it.\n\nWatch next:\n- Verify market-data freshness.\n- Check rates, sector breadth, and macro-release details before upgrading confidence.",
                str(d / "manifest.json"),
            ))
    return out


def news_examples() -> list[dict[str, Any]]:
    out = []
    for path in sorted((ROOT / "tra-ai-news-bot-v1/state").glob("last_analysis_*.json")):
        data = load_json(path) or {}
        category = path.stem.replace("last_analysis_", "")
        blob = json.dumps(data, ensure_ascii=False)[:7000]
        if not blob or blob == "{}":
            continue
        out.append(example(
            "news_cluster_summary",
            f"Summarize this {category} news-analysis state for Mike. Keep it source-aware, concise, and avoid overstating stale/fallback data.\n\nJSON:\n{blob}",
            f"I would treat this as a {category} snapshot, not ground truth. Key points should be grouped by recurring themes, with any fallback/stale fields called out explicitly. If a model or source failed, say so rather than smoothing it over. Use concise bullets and link back to the underlying dashboard/source artifacts where available.",
            str(path),
        ))
    for path in sorted((ROOT / "tra-ai-news-bot-v1/state").glob("top_snapshot_*.json")):
        data = load_json(path) or {}
        items = data.get("items") or data.get("top") or data.get("articles") or []
        if not items:
            continue
        titles = []
        for it in items[:8]:
            if isinstance(it, dict):
                titles.append(str(it.get("title") or it.get("headline") or it)[:180])
        if titles:
            out.append(example(
                "news_priority_selection",
                "Pick the most important stories from this snapshot and explain why, without clickbait.\n\n" + "\n".join(f"- {t}" for t in titles),
                "Prioritize stories that change decisions, reveal durable trends, or affect Mike's active projects. De-emphasize duplicate headlines, generic market noise, and unsupported claims. Return 3–5 bullets with a short why-it-matters note for each.",
                str(path),
            ))
    return out


def ops_examples() -> list[dict[str, Any]]:
    out = []
    # Hand-authored from actual OpenClaw ops decisions.
    patterns = [
        ("Qwen is running at 64K on claw1. A Gemma EOD job is requested while Qwen is serving chat and news summaries. What should you do?",
         "Check queue/resource state first. If Gemma is required, acquire/hold the relevant AI Resource Queue lease, warn that swapping models can interrupt Qwen and may take several minutes to restore, then stop conflicting Qwen/Gemma containers only through the existing orchestration path. Prefer scheduling Gemma EOD in a low-traffic window; do not kill containers casually."),
        ("A finance workflow asks for current market data, but Stooq times out and local price_cache.json exists. How should the report handle this?",
         "Use the local cache only if it is the best available source, disclose that external retrieval failed, include cache timestamp/source in the summary or manifest, and lower confidence. Do not invent fresh prices or hide the retrieval failure."),
        ("A user-facing dashboard link currently uses 127.0.0.1. Mike will access it through VPN. What should you do?",
         "Avoid hardcoded localhost/IP links in user-facing output. Prefer relative links or current-browser-host rewrites; if an address is unavoidable, use the reachable VPN/server address and explain it briefly."),
        ("A command produces huge logs but might contain important raw evidence. Should you use sqz?",
         "Use the opt-in `scripts/sqz-safe` wrapper only when helpful. Preserve raw stdout/stderr artifacts, avoid global sqz hooks, and never rely on compressed output as the only evidence."),
        ("A cron/background agent will edit workspace files and run builds. What should happen first?",
         "Acquire an AI Resource Queue external lease for the workspace/resource group, make the work visible in `/api/resources`, release the lease on completion or failure, and log artifacts/validation. Do not let detached agents silently collide."),
    ]
    for i, (u, a) in enumerate(patterns, 1):
        out.append(example("ops_routing", u, a, f"hand-authored:ops:{i}"))
    for path in [ROOT / "reports/claw1-spark-video-config-2026-05-11.md", ROOT / "reports/sqz-cli-audit-2026-05-12.md", ROOT / "documentation/AI_RESOURCE_QUEUE.md", ROOT / "documentation/finance-newsletter/WORKFLOW.md"]:
        txt = read(path, 5000)
        if txt:
            out.append(example(
                "ops_report_to_action",
                "Read this operations note and extract the durable rule future-you should follow.\n\n" + txt[:4200],
                "Extract the operational rule, the validation evidence, and the failure mode to avoid. Keep secrets out. If the note describes a resource conflict, prefer queue/lease orchestration over ad hoc process kills. End with one concrete next action or 'no action needed'.",
                str(path),
            ))
    return out


def preference_examples() -> list[dict[str, Any]]:
    user = read(ROOT / "USER.md", 3000)
    tools = read(ROOT / "TOOLS.md", 3000)
    soul = read(ROOT / "SOUL.md", 2500)
    out = []
    out.append(example(
        "preference_alignment",
        "Given these local preference notes, summarize the behavioral rules that matter most when helping Mike.\n\n" + user + "\n\n" + tools,
        "Key rules: be local-first and cache useful source material; avoid hardcoded localhost/IPs in user-facing links; respect legal/ToS boundaries; do not expose tokens or secrets in docs/UI/logs; prefer LM Studio/claw routes over Ollama assumptions; use queue/resource coordination for GPU-heavy work; keep dashboard navigation tidy; document durable work and validation evidence.",
        "USER.md+TOOLS.md",
    ))
    out.append(example(
        "tone_style",
        "Rewrite this assistant behavior into the desired T'Ra style.\n\n" + soul,
        "Be useful without filler, concise unless depth matters, direct about risk, and calm when things break. Act when safe, ask only for genuinely blocking decisions, and leave a durable trail of what changed, why, and how it was validated.",
        "SOUL.md",
    ))
    return out


def report_cleanup_examples() -> list[dict[str, Any]]:
    bad_table = """Market Dashboard\n| Asset | Start | Latest | Week | Source |\n|---|---:|---:|---:|---|\n| S&P 500 (SPY) | 720.49 | 738.19 | 2.46% | local macro bot price_cache.json |\n| Nasdaq 100 (QQQ) | 674.10 | 707.22 | 4.91% | local macro bot price_cache.json |"""
    good = """### Market Dashboard\n\n| Asset | Start | Latest | Week | Source |\n|---|---:|---:|---:|---|\n| S&P 500 (SPY) | 720.49 | 738.19 | +2.46% | local macro bot price_cache.json |\n| Nasdaq 100 (QQQ) | 674.10 | 707.22 | +4.91% | local macro bot price_cache.json |\n\nData note: these values came from the local macro cache, so the report should disclose cache/source freshness if external verification failed."""
    return [example(
        "report_cleanup",
        "This section rendered poorly in a PDF as wrapped pipe text. Rewrite it as a clean report-ready Markdown table and include any needed caveat.\n\n" + bad_table,
        good,
        "hand-authored:finance-table-fix",
    )]



def macro_examples() -> list[dict[str, Any]]:
    out = []
    reports = sorted((ROOT / "tra-macro-intel-bot-v1/site/reports").glob("2026-*.md"))[-30:]
    for path in reports:
        txt = read(path, 4500)
        if not txt:
            continue
        out.append(example(
            "macro_brief",
            "Turn this macro report into a concise Mike-ready brief. Separate signal, uncertainty, and watch-next. Do not invent market data.\n\n" + txt[:3800],
            "Signal:\n- Identify the strongest macro regime indicators that are explicitly supported by the report.\n- Connect them to likely market implications only when the source material supports that link.\n\nUncertainty:\n- Flag stale, missing, or weakly sourced metrics.\n- Avoid treating one-day moves as durable regime changes.\n\nWatch next:\n- Check inflation, rates, liquidity, credit stress, FX, and sector breadth for confirmation.\n- Preserve source dates and data freshness in the final brief.",
            str(path),
        ))
    return out


def failure_examples() -> list[dict[str, Any]]:
    cases = [
        ("vLLM returns HTTP 400: max_tokens cannot be greater than max_model_len. The user asked why a large output failed.",
         "Explain that the server enforces total context length: prompt tokens plus requested output tokens must fit inside max_model_len. Fix by dynamically clamping max_tokens based on estimated prompt size, then validate with a marker request. Do not claim the model itself is limited if the serving flag is the actual cap."),
        ("SSH to claw1 accepts TCP but times out during banner exchange while GPU jobs are running.",
         "Treat the host as saturated, not necessarily offline. Check non-SSH health endpoints, queue jobs, and tunnels first. Avoid reboot/kill unless explicitly approved or a queue-managed recovery path exists."),
        ("A generated dashboard HTML page needs a UI change.",
         "Do not edit generated HTML directly. Find and patch the source generator/template, regenerate, then validate the rendered page."),
        ("A finance report has a confident sentence but the supporting source timed out.",
         "Downgrade confidence, disclose the timeout, use cached/local data only with source timestamp, and avoid causal claims not supported by available evidence."),
        ("A user says 'ping' after a long-running task stalled.",
         "Acknowledge promptly, apologize briefly if the stall was ours, give concrete current state, and continue or state the blocker. Do not pretend nothing happened."),
        ("A second GPU node is expected soon; current node is swapping Qwen/Gemma slowly.",
         "Prefer node-role separation: keep Qwen warm on one node for chat/news, use the second node for Gemma/EOD/training experiments, and let the AI Resource Queue route based on health and resource leases."),
    ]
    return [example("failure_handling", u, a, f"hand-authored:failure:{i}") for i, (u, a) in enumerate(cases, 1)]

def build() -> list[dict[str, Any]]:
    rows = []
    for fn in [finance_examples, news_examples, macro_examples, ops_examples, failure_examples, preference_examples, report_cleanup_examples]:
        rows.extend(fn())
    # Deduplicate by user+assistant pair.
    seen = set(); unique = []
    for r in rows:
        key = (r["messages"][1]["content"], r["messages"][2]["content"])
        if key not in seen:
            seen.add(key); unique.append(r)
    return unique


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "training/ministral14b/data/openclaw-sft-v0"))
    ap.add_argument("--eval-ratio", type=float, default=0.2)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)
    rows = build()
    Random(args.seed).shuffle(rows)
    n_eval = max(1, int(len(rows) * args.eval_ratio)) if len(rows) > 3 else 1
    eval_rows = rows[:n_eval]
    train_rows = rows[n_eval:]
    for name, part in [("train.jsonl", train_rows), ("eval.jsonl", eval_rows), ("all.jsonl", rows)]:
        with (out / name).open("w") as f:
            for r in part:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
    summary = {
        "total": len(rows),
        "train": len(train_rows),
        "eval": len(eval_rows),
        "categories": {},
        "precision_policy": "Dataset for BF16/8-bit-or-better LoRA; no 4-bit training path.",
    }
    for r in rows:
        summary["categories"][r["category"]] = summary["categories"].get(r["category"], 0) + 1
    (out / "DATASET_CARD.md").write_text("# OpenClaw SFT v0\n\n" + json.dumps(summary, indent=2) + "\n\nSources are local artifacts; review before real training.\n")
    print(json.dumps(summary, indent=2))

if __name__ == "__main__":
    main()
