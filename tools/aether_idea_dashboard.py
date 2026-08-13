#!/usr/bin/env python3
"""Live dashboard for an Aether idea-miner run.

Reads the checkpointed JSON that ``tools/aether_idea_miner.py`` writes after
every program (it already carries a fresh ``summary`` and ranked ``findings``)
and presents run status — per-model progress, totals, the live findings board,
and recent activity. Three modes:

  --watch  (default)  live terminal view, refreshed every --interval seconds
  --once               print one snapshot and exit (good for scripts / a peek)
  --serve [PORT]       serve an auto-refreshing HTML dashboard in the browser

It only ever READS the report file (atomic writes from the miner make that
safe), so you can point it at a run in progress or a finished one. Run it in a
second terminal / tmux pane next to a detached miner:

  python3 tools/aether_idea_dashboard.py /path/to/report.json            # terminal
  python3 tools/aether_idea_dashboard.py /path/to/report.json --serve    # browser :8077
"""

from __future__ import annotations

import argparse
import http.server
import json
import pathlib
import socketserver
import sys
import time
from typing import Any

# Reuse the miner's finding headline / action / glossary so the dashboard and
# the report stay consistent. Import is side-effect-free.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_idea_miner as m  # noqa: E402


# --------------------------------------------------------------------------- #
# Load + view model
# --------------------------------------------------------------------------- #
def load_report(path: pathlib.Path) -> dict[str, Any] | None:
    """Read the report. Returns None if it does not exist yet; tolerates a brief
    mid-write race by retrying once."""
    if not path.exists():
        return None
    for _ in range(3):
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            time.sleep(0.15)
    return None


def model_status(record: dict[str, Any] | None) -> str:
    if record is None:
        return "pending"
    if record.get("error"):
        return "error"
    if "stats" in record:
        return "done"
    return "running"


def parse_iso(ts: str | None) -> float | None:
    if not ts:
        return None
    try:
        return time.mktime(time.strptime(ts, "%Y-%m-%dT%H:%M:%S"))
    except Exception:
        return None


def fmt_duration(seconds: float | None) -> str:
    if seconds is None or seconds < 0:
        return "—"
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    mnt, sec = divmod(rem, 60)
    if h:
        return f"{h}h{mnt:02d}m"
    if mnt:
        return f"{mnt}m{sec:02d}s"
    return f"{sec}s"


def build_view(report: dict[str, Any], report_path: pathlib.Path) -> dict[str, Any]:
    models = report.get("models", [])
    by_id = {mr.get("destination_id"): mr for mr in models}
    # Prefer the authoritative planned list (newer reports). Fall back to the
    # destinations config the report points at (so reports written before that
    # field existed still show pending models), then to started-only.
    planned = report.get("planned_destinations")
    if not planned:
        cfg_path = report.get("destinations_config")
        if cfg_path:
            try:
                raw = json.loads(pathlib.Path(cfg_path).read_text(encoding="utf-8"))
                planned = [d["id"] for d in raw.get("destinations", []) if isinstance(d, dict) and d.get("id")]
            except Exception:
                planned = None
    if not planned:
        planned = [mr.get("destination_id") for mr in models]
    # Ensure every started model appears even if it is not in the planned list.
    for mr in models:
        if mr.get("destination_id") not in planned:
            planned.append(mr.get("destination_id"))
    per_model = max(1, int(report.get("programs_per_model", 1))) * max(1, int(report.get("rounds", 1)))

    rows: list[dict[str, Any]] = []
    for did in planned:
        rec = by_id.get(did)
        status = model_status(rec)
        progs = rec.get("programs", []) if rec else []
        done = len(progs)
        success = sum(1 for p in progs if p.get("success"))
        fixed = sum(1 for p in progs if p.get("fixed_by_repair"))
        last_intent = ""
        for p in reversed(progs):
            if p.get("intent"):
                last_intent = p["intent"]
                break
        rows.append({
            "destination_id": did,
            "model": (rec or {}).get("model") or "",
            "guide": (rec or {}).get("guide") or "",
            "status": status,
            "done": done,
            "expected": per_model,
            "success": success,
            "fixed": fixed,
            "failed": done - success,
            "error": (rec or {}).get("error", ""),
            "last_intent": last_intent,
            "notes": (rec or {}).get("notes") or [],
        })

    started = sum(1 for r in rows if r["status"] != "pending")
    done_models = sum(1 for r in rows if r["status"] in ("done", "error"))

    # Recent activity: last programs across all models, in chronological order.
    recent: list[dict[str, Any]] = []
    for mr in models:
        for p in mr.get("programs", []):
            ff = p.get("initial_failure") or {}
            recent.append({
                "model": mr.get("model") or mr.get("destination_id"),
                "intent": p.get("intent") or "(unspecified)",
                "success": p.get("success"),
                "fixed": p.get("fixed_by_repair"),
                "code": ff.get("code") or ("runtime" if ff.get("phase") == "runtime"
                                           else ("silent" if ff.get("silent") else "")),
            })

    start_epoch = parse_iso(report.get("generated_at"))
    try:
        mtime = report_path.stat().st_mtime
    except OSError:
        mtime = None
    now = time.time()

    summary = report.get("summary", {}) or {}
    run_complete = done_models >= len(planned) and len(planned) > 0

    return {
        "report_path": str(report_path),
        "aether_version": report.get("aether_version", "?"),
        "generated_at": report.get("generated_at", "?"),
        "elapsed": (now - start_epoch) if start_epoch else None,
        "since_update": (now - mtime) if mtime else None,
        "per_model": per_model,
        "repair_attempts": report.get("repair_attempts"),
        "temperature": report.get("temperature"),
        "planned_count": len(planned),
        "started_models": started,
        "done_models": done_models,
        "run_complete": run_complete,
        "rows": rows,
        "summary": summary,
        "findings": report.get("findings", []) or [],
        "recent": recent,
    }


# --------------------------------------------------------------------------- #
# Terminal rendering
# --------------------------------------------------------------------------- #
class C:
    """ANSI helpers; blanked when output is not a TTY / --no-color."""
    enabled = True

    @classmethod
    def w(cls, code: str, text: str) -> str:
        if not cls.enabled:
            return text
        return f"\033[{code}m{text}\033[0m"


def c_dim(t): return C.w("2", t)
def c_bold(t): return C.w("1", t)
def c_green(t): return C.w("32", t)
def c_red(t): return C.w("31", t)
def c_yellow(t): return C.w("33", t)
def c_cyan(t): return C.w("36", t)
def c_blue(t): return C.w("34", t)
def c_mag(t): return C.w("35", t)


STATUS_GLYPH = {
    "done": lambda: c_green("✓ done   "),
    "running": lambda: c_yellow("● running"),
    "error": lambda: c_red("✗ error  "),
    "pending": lambda: c_dim("· pending"),
}


def bar(done: int, total: int, width: int = 16, color=c_cyan) -> str:
    total = max(total, 1)
    filled = min(width, round(width * done / total))
    return color("█" * filled) + c_dim("░" * (width - filled))


def render_terminal(view: dict[str, Any], width: int = 92) -> str:
    out: list[str] = []
    title = " Aether idea-miner — live dashboard "
    out.append(c_bold(c_cyan("╭" + "─" * (width - 2) + "╮")))
    head = f"aether {view['aether_version']}  ·  started {view['generated_at']}  ·  elapsed {fmt_duration(view['elapsed'])}"
    out.append(c_bold(c_cyan("│")) + c_bold(title.ljust(width - 2)[:width - 2]) + c_bold(c_cyan("│")))
    out.append(c_bold(c_cyan("│")) + c_dim(("  " + head).ljust(width - 2)[:width - 2]) + c_bold(c_cyan("│")))
    if view["run_complete"]:
        live = c_green("complete")
    else:
        ago = fmt_duration(view["since_update"]) if view["since_update"] is not None else "—"
        live = c_yellow("running") + c_dim(f" (updated {ago} ago)")
    sub = f"  models {view['done_models']}/{view['planned_count']} done  ·  {view['per_model']} programs/model  ·  "
    line = sub + live
    pad = max(0, (width - 2) - _vislen(line))
    out.append(c_bold(c_cyan("│")) + line + " " * pad + c_bold(c_cyan("│")))
    out.append(c_bold(c_cyan("╰" + "─" * (width - 2) + "╯")))

    s = view["summary"]
    pt = s.get("programs_total", 0)
    ps = s.get("programs_success", 0)
    out.append("")
    out.append("  " + c_bold("Overall  ") + bar(ps, pt or 1, 24, c_green) +
               f"  {c_green(str(ps))}/{pt} ran clean"
               f"  ·  {c_yellow(str(s.get('fixed_by_repair', 0)))} fixed by repair"
               f"  ·  {c_red(str(s.get('programs_failed', 0)))} failed"
               f"  ·  {c_mag(str(s.get('finding_count', 0)))} findings")

    # Per-model table.
    out.append("")
    out.append(c_bold("  Models"))
    out.append(c_dim("  " + "model".ljust(26) + "guide  status     progress           clean fix fail"))
    for r in view["rows"]:
        glyph = STATUS_GLYPH.get(r["status"], STATUS_GLYPH["pending"])()
        prog = bar(r["done"], r["expected"], 12) + f" {r['done']:>2}/{r['expected']:<2}"
        name = (r["model"] or r["destination_id"])[:25].ljust(26)
        line = (f"  {name}{(r['guide'] or '-'):<6} {glyph}  {prog}  "
                f"{c_green(str(r['success'])):>3} {c_yellow(str(r['fixed'])):>3} {c_red(str(r['failed'])):>3}")
        out.append(line)
        if r["status"] == "running" and r["last_intent"]:
            out.append(c_dim("      ↳ " + r["last_intent"][:width - 10]))
        if r["status"] == "error":
            out.append(c_red("      ! " + (r["error"] or "")[:width - 10]))
        for note in r["notes"][:1]:
            out.append(c_dim("      ⚠ " + str(note)[:width - 10]))

    # Findings board.
    out.append("")
    out.append(c_bold("  Findings ") + c_dim("(ranked by distinct-model breadth)"))
    findings = view["findings"]
    if not findings:
        out.append(c_dim("    none yet — every program has compiled+run so far"))
    else:
        out.append(c_dim("    #  models  occ  unrep  finding"))
        for i, f in enumerate(findings[:10], 1):
            mc = f.get("model_count", 0)
            breadth = c_green if mc >= 3 else (c_yellow if mc >= 2 else c_dim)
            head = m.finding_headline(f)
            out.append(f"    {i:<2} {breadth(str(mc)):>5}  {f.get('occurrences',0):>3}  "
                       f"{f.get('unrepaired',0):>4}   {head[:width - 26]}")

    # Recent activity.
    recent = view["recent"][-6:]
    if recent:
        out.append("")
        out.append(c_bold("  Recent"))
        for r in recent:
            if r["success"] and r["fixed"]:
                tag = c_yellow("fixed")
            elif r["success"]:
                tag = c_green("ok   ")
            else:
                tag = c_red("FAIL ")
            code = c_dim(f"[{r['code']}]") if r["code"] else "      "
            out.append(f"    {tag} {code} {c_dim(r['model'][:18]):<18} {r['intent'][:width - 40]}")

    out.append("")
    out.append(c_dim(f"  report: {view['report_path']}"))
    return "\n".join(out)


def _vislen(s: str) -> int:
    """Visible length ignoring ANSI escapes (for rough padding)."""
    import re
    return len(re.sub(r"\033\[[0-9;]*m", "", s))


# --------------------------------------------------------------------------- #
# Browser server
# --------------------------------------------------------------------------- #
HTML_PAGE = """<!doctype html><html><head><meta charset="utf-8">
<title>Aether idea-miner dashboard</title>
<style>
:root{--bg:#0d1117;--fg:#c9d1d9;--dim:#8b949e;--card:#161b22;--line:#30363d;
--grn:#3fb950;--ylw:#d29922;--red:#f85149;--cyn:#39c5cf;--mag:#bc8cff;--blu:#58a6ff}
*{box-sizing:border-box}body{background:var(--bg);color:var(--fg);font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;margin:0;padding:24px}
h1{font-size:18px;margin:0 0 4px}.sub{color:var(--dim);font-size:13px;margin-bottom:18px}
.badge{padding:2px 8px;border-radius:10px;font-size:12px;font-weight:600}
.live{background:rgba(63,185,80,.15);color:var(--grn)}.idle{background:rgba(210,153,34,.15);color:var(--ylw)}
.done{background:rgba(57,197,207,.15);color:var(--cyn)}
.grid{display:grid;grid-template-columns:repeat(5,1fr);gap:12px;margin-bottom:20px}
.kpi{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px}
.kpi .n{font-size:24px;font-weight:700}.kpi .l{color:var(--dim);font-size:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:14px 16px;margin-bottom:18px}
.card h2{font-size:14px;margin:0 0 10px;color:var(--dim);text-transform:uppercase;letter-spacing:.05em}
table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line);font-size:13px}
th{color:var(--dim);font-weight:600}
.barwrap{display:inline-block;width:120px;height:9px;background:#21262d;border-radius:5px;overflow:hidden;vertical-align:middle;margin-right:8px}
.barfill{height:100%;background:var(--cyn)}
.s-done{color:var(--grn)}.s-running{color:var(--ylw)}.s-error{color:var(--red)}.s-pending{color:var(--dim)}
.g{color:var(--grn)}.y{color:var(--ylw)}.r{color:var(--red)}.m{color:var(--mag)}.d{color:var(--dim)}.c{color:var(--cyn)}
.pill{display:inline-block;min-width:22px;text-align:center;padding:1px 7px;border-radius:9px;background:#21262d;font-weight:600}
.intent{color:var(--dim);font-size:12px}
.tag{font-weight:700;margin-right:6px}
</style></head><body>
<h1>Aether idea-miner <span class="d">·</span> <span id="ver"></span></h1>
<div class="sub" id="meta">connecting…</div>
<div class="grid" id="kpis"></div>
<div class="card"><h2>Models</h2><table id="models"></table></div>
<div class="card"><h2>Findings — ranked by distinct-model breadth</h2><table id="findings"></table></div>
<div class="card"><h2>Recent</h2><table id="recent"></table></div>
<script>
const INTERVAL=__INTERVAL__;
function esc(s){return (s||'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}
function barHTML(d,t){let p=Math.round(100*Math.min(d,t)/Math.max(t,1));return `<span class="barwrap"><span class="barfill" style="width:${p}%"></span></span>${d}/${t}`}
function statusCls(s){return 's-'+s}
function headline(f){
  if(f.kind==='missing_construct')return 'Reached for <code>'+esc(f.identifier)+'</code> — not in scope';
  if(f.kind==='error_code')return 'Tripped on <code>'+esc(f.code)+'</code>';
  if(f.kind==='runtime')return 'Runtime error';
  if(f.kind==='silent')return 'Silent failure (no diagnostic)';
  return esc(f.key);}
async function tick(){
  let r; try{r=await (await fetch('data?_='+Date.now())).json();}catch(e){document.getElementById('meta').textContent='waiting for report…';return;}
  const v=r.view, s=v.summary||{};
  document.getElementById('ver').textContent='aether '+v.aether_version;
  let liveCls='live',liveTxt='running';
  if(v.run_complete){liveCls='done';liveTxt='complete';}
  const ago=v.since_update==null?'':' · updated '+(v.since_update<60?v.since_update+'s':Math.round(v.since_update/60)+'m')+' ago';
  document.getElementById('meta').innerHTML=`started ${v.generated_at} · elapsed ${v.elapsed_h} · models ${v.done_models}/${v.planned_count} · ${v.per_model} programs/model${ago} · <span class="badge ${liveCls}">${liveTxt}</span>`;
  const kpi=[['programs',s.programs_total||0,'c'],['ran clean',s.programs_success||0,'g'],['fixed by repair',s.fixed_by_repair||0,'y'],['failed',s.programs_failed||0,'r'],['findings',s.finding_count||0,'m']];
  document.getElementById('kpis').innerHTML=kpi.map(k=>`<div class="kpi"><div class="n ${k[2]}">${k[1]}</div><div class="l">${k[0]}</div></div>`).join('');
  document.getElementById('models').innerHTML='<tr><th>model</th><th>guide</th><th>status</th><th>progress</th><th>clean</th><th>fix</th><th>fail</th></tr>'+
    v.rows.map(x=>`<tr><td>${esc(x.model||x.destination_id)}${x.status==='running'&&x.last_intent?'<div class="intent">↳ '+esc(x.last_intent)+'</div>':''}${x.status==='error'?'<div class="intent r">! '+esc(x.error)+'</div>':''}</td><td class="d">${x.guide||'-'}</td><td class="${statusCls(x.status)}">${x.status}</td><td>${barHTML(x.done,x.expected)}</td><td class="g">${x.success}</td><td class="y">${x.fixed}</td><td class="r">${x.failed}</td></tr>`).join('');
  const fs=v.findings||[];
  document.getElementById('findings').innerHTML= fs.length? '<tr><th>#</th><th>models</th><th>occ</th><th>unrep</th><th>finding</th><th>code</th></tr>'+
    fs.slice(0,12).map((f,i)=>`<tr><td class="d">${i+1}</td><td><span class="pill ${f.model_count>=3?'g':(f.model_count>=2?'y':'d')}">${f.model_count}</span></td><td>${f.occurrences}</td><td>${f.unrepaired}</td><td>${headline(f)}</td><td class="d">${esc(f.code||'')}</td></tr>`).join('')
    : '<tr><td class="d">none yet — everything has compiled+run so far</td></tr>';
  const rec=(v.recent||[]).slice(-12).reverse();
  document.getElementById('recent').innerHTML=rec.map(x=>{let t=x.success?(x.fixed?'<span class="tag y">fixed</span>':'<span class="tag g">ok</span>'):'<span class="tag r">FAIL</span>';return `<tr><td>${t}</td><td class="d">${esc(x.code||'')}</td><td class="d">${esc(x.model)}</td><td class="intent">${esc(x.intent)}</td></tr>`}).join('');
}
tick();setInterval(tick,INTERVAL*1000);
</script></body></html>"""


def make_handler(report_path: pathlib.Path, interval: int):
    page = HTML_PAGE.replace("__INTERVAL__", str(interval))

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def _send(self, code, body, ctype):
            data = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.startswith("/data"):
                report = load_report(report_path)
                if report is None:
                    self._send(200, json.dumps({"view": None}), "application/json")
                    return
                view = build_view(report, report_path)
                # add a couple display-friendly fields
                view["elapsed_h"] = fmt_duration(view["elapsed"])
                self._send(200, json.dumps({"view": view}), "application/json")
                return
            self._send(200, page, "text/html; charset=utf-8")

    return Handler


def serve(report_path: pathlib.Path, port: int, interval: int) -> int:
    handler = make_handler(report_path, interval)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), handler) as httpd:
        print(f"Aether idea-miner dashboard: http://127.0.0.1:{port}/  "
              f"(report: {report_path}, refresh {interval}s) — Ctrl-C to stop")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped.")
    return 0


# --------------------------------------------------------------------------- #
# Watch / once
# --------------------------------------------------------------------------- #
def render_once(report_path: pathlib.Path) -> str:
    report = load_report(report_path)
    if report is None:
        return c_yellow(f"no report yet at {report_path} (waiting for the miner's first checkpoint)")
    return render_terminal(build_view(report, report_path))


def watch(report_path: pathlib.Path, interval: int) -> int:
    try:
        while True:
            frame = render_once(report_path)
            sys.stdout.write("\033[2J\033[H")  # clear + home
            sys.stdout.write(frame + "\n")
            sys.stdout.flush()
            report = load_report(report_path)
            if report is not None:
                view = build_view(report, report_path)
                if view["run_complete"]:
                    sys.stdout.write(c_green("\n  run complete.\n"))
                    sys.stdout.flush()
                    return 0
            time.sleep(interval)
    except KeyboardInterrupt:
        return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("report", type=pathlib.Path, help="path to the miner's --output-json report")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--watch", action="store_true", help="live terminal view (default)")
    mode.add_argument("--once", action="store_true", help="print one snapshot and exit")
    mode.add_argument("--serve", action="store_true", help="serve a browser dashboard")
    p.add_argument("--port", type=int, default=8077, help="port for --serve (default: 8077)")
    p.add_argument("--interval", type=int, default=3, help="refresh seconds (default: 3)")
    p.add_argument("--no-color", action="store_true", help="disable ANSI color")
    args = p.parse_args()

    C.enabled = (not args.no_color) and sys.stdout.isatty()

    if args.serve:
        return serve(args.report, args.port, max(1, args.interval))
    if args.once:
        print(render_once(args.report))
        return 0
    return watch(args.report, max(1, args.interval))


if __name__ == "__main__":
    raise SystemExit(main())
