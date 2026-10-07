#!/usr/bin/env python3
"""Check (and re-pin) the repair-drill overlays against the live compiler.

Runs every drill's broken_source through the given aether binary, the same
check tools/aether_specialization_build_dataset.py applies on every build:

- a `kind: repair` drill must still be rejected, and the first code it emits
  must equal the code its stored `diagnostic` cites;
- a `kind: behavioral` drill must run, print something other than its
  expected stdout, and emit the stored code if it cites one (a warning).

Default (check) mode prints every drill whose polarity flipped ("drill
obsolete"), whose code drifted ("code drift X->Y") or whose stored text
differs from what the compiler prints today, and exits 1 on any polarity
failure or code drift. Text drift alone is advisory: the dataset prompt
already uses the live stderr.

  --write                re-pin the stored `diagnostic` of every drill whose
                         code still matches to the compiler's current first
                         diagnostic line (location prefix stripped)
  --accept-code-drift    with --write: also re-pin drills whose code drifted,
                         after you have checked that the new code is the right
                         diagnostic for the broken program

A polarity failure is never re-pinned: the drill teaches the wrong thing and
has to be deleted or repointed by hand. Run this in the gitlink-bump commit of
every release, and whenever a CHANGELOG entry legalizes a construct.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import aether_specialization_build_dataset as bd  # noqa: E402
import aether_specialization_corpus_policy as policy  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SEED_DIR = REPO_ROOT / "Tests" / "aether_specialization"
DEFAULT_OVERLAYS = [
    SEED_DIR / "seed_repair_pairs.json",
    SEED_DIR / "seed_repair_pairs.qwen25.json",
    SEED_DIR / "seed_repair_pairs.granite.json",
]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("overlays", nargs="*", type=pathlib.Path, default=DEFAULT_OVERLAYS)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=bd.DEFAULT_AETHER_BIN)
    parser.add_argument("--write", action="store_true",
                        help="re-pin stored diagnostics whose code still matches")
    parser.add_argument("--accept-code-drift", action="store_true",
                        help="with --write: also re-pin drills whose code drifted")
    parser.add_argument("--report-json", type=pathlib.Path, default=None)
    args = parser.parse_args()

    if args.accept_code_drift and not args.write:
        parser.error("--accept-code-drift only makes sense with --write")
    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")
    stamp = policy.aether_identity(args.aether_bin)
    print(f"aether {stamp['aether_version']} ({stamp['aether_sha256'][:12]})")

    report: dict[str, dict] = {}
    polarity_failures = code_drifts = text_drifts = repinned = drills = 0
    for overlay in args.overlays:
        payload = json.loads(overlay.read_text(encoding="utf-8"))
        rows = []
        changed = False
        for item in payload.get("pairs", []):
            drills += 1
            broken = bd.probe_broken_source(args.aether_bin, item)
            problems = bd.drill_problems(item, broken)
            polarity = [p for p in problems if not p.startswith("code drift")]
            drift = [p for p in problems if p.startswith("code drift")]
            live_line = broken["first_line"]
            stored = item.get("diagnostic", "")
            text_drift = bool(live_line) and live_line != stored.strip()
            action = ""
            if args.write and text_drift and not polarity and (not drift or args.accept_code_drift):
                item["diagnostic"] = live_line
                changed = True
                repinned += 1
                action = "re-pinned"
            polarity_failures += bool(polarity)
            code_drifts += bool(drift) and action != "re-pinned"
            text_drifts += text_drift and action != "re-pinned"
            rows.append({
                "id": item.get("id"),
                "kind": item.get("kind"),
                "broken_returncode": broken["returncode"],
                "stored_code": bd.stored_drill_code(stored),
                "live_code": broken["first_code"],
                "problems": problems,
                "text_drift": text_drift,
                "stored_diagnostic": stored,
                "live_diagnostic": live_line,
                "action": action,
            })
            if problems or text_drift:
                status = "; ".join(problems) if problems else "text drift"
                print(f"  {overlay.name}: {item.get('id')}: {status}{' -> ' + action if action else ''}")
                if text_drift:
                    print(f"      stored: {stored}")
                    print(f"      live:   {live_line}")
        if changed:
            overlay.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        report[policy.display_path(overlay)] = {"drills": rows, "written": changed}

    print(
        f"drills={drills} polarity_failures={polarity_failures} code_drifts={code_drifts} "
        f"text_drifts={text_drifts} re-pinned={repinned}"
    )
    if args.report_json:
        args.report_json.write_text(
            json.dumps({**stamp, "overlays": report}, indent=2) + "\n", encoding="utf-8"
        )
    return 1 if polarity_failures or code_drifts else 0


if __name__ == "__main__":
    raise SystemExit(main())
