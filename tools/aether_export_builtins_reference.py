#!/usr/bin/env python3
"""Export the Aether builtin surface as a training-corpus reference.

Runs the `aether` compiler's own `builtins_json(true)` probe, drops the
SDL/graphics-demo categories Aether superficially hides (graphics, 3d, user),
and emits a reference document for the specialization corpus:

  * fully-documented builtins (signature + return type + effectful + usage),
    grouped by category, for the Aether-first-class surface; and
  * the remaining builtin *names* by category, so generated code knows they
    exist and does not invent them; and
  * a "Discovering builtins" section stating the surface is queryable at
    runtime (`builtins_json` / `builtin_info` / `aether --dump-ext-builtins`) --
    the bridge for every builtin not fully documented here.

The exclusion is category-based and logged, never silent, so it stays auditable
as the host's registered builtins change.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

# Categories that are SDL-backed graphics / windowing / demo host builtins.
# Aether does not expose these as part of its surface, so they stay out of the
# training corpus. Category-based and explicit so it is easy to audit/adjust.
DEFAULT_EXCLUDE = ("graphics", "3d", "user")

# Some host builtins leak into the "core" category (SDL demos, legacy Pascal
# units, VM/introspection plumbing) that are not part of the data-automation
# surface Aether targets. Drop them so the corpus teaches the clean surface.
# All curated explicitly (by prefix or name) so the exclusion stays auditable.
EXCLUDE_NAME_PREFIXES = (
    "landscape",   # SDL landscape renderer (miscategorized as core)
    "dos",         # DOS unit: dosExec/dosFindfirst/dosGetdate/...
    "extbuiltin",  # extended-builtin introspection counters
)

# Pascal CRT unit -- terminal/cursor/colour control; not Aether automation.
_CRT = {
    "clrscr", "clreol", "gotoxy", "wherex", "wherey", "cursoroff", "cursoron",
    "hidecursor", "showcursor", "savecursor", "restorecursor", "highvideo",
    "lowvideo", "normvideo", "normalcolors", "invertcolors", "textbackground",
    "textbackgrounde", "textcolor", "textcolore", "blinktext", "boldtext",
    "underlinetext", "window", "insline", "deline", "pushscreen", "popscreen",
    "screencols", "screenrows", "beep", "keypressed", "readkey", "quitrequested",
    "biblinktext", "biboldtext", "biclrscr", "bilowvideo", "binormvideo",
    "biunderlinetext", "biwherex", "biwherey",
}
# DOS file-search helpers not caught by the "dos" prefix.
_DOS = {"findfirst", "findnext", "getfattr"}
# VM/introspection plumbing and redundant non-canonical alias spellings
# (canonical: builtin_info/builtins_json; thread_* with underscores).
_INTERNAL = {
    "aetherbuiltininfo", "aetherbuiltinsjson", "hasextbuiltin",
    "vmversion", "bytecodeversion",
    "threadcancel", "threadgetresult", "threadgetstatus", "threadlookup",
    "threadpause", "threadpoolsubmit", "threadresume", "threadsetname",
    "threadspawnbuiltin", "threadstats", "threadstatsjson",
}
# Registry placeholder / junk entries that are not real callable builtins.
EXCLUDE_NAMES = {"to be filled", "to_be_filled"} | _CRT | _DOS | _INTERNAL

PROBE = 'fn main() -> Void {\n    fx {\n        println(builtins_json(true));\n    }\n    ret;\n}\n'


def query_builtins(aether_bin: pathlib.Path) -> list[dict]:
    with tempfile.NamedTemporaryFile("w", suffix=".ae", delete=False) as fh:
        fh.write(PROBE)
        probe_path = fh.name
    out = subprocess.run([str(aether_bin), probe_path], capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        sys.exit(f"aether probe failed (rc={out.returncode}): {out.stderr[:400]}")
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError as exc:
        sys.exit(f"could not parse builtins_json output: {exc}")


def _excluded(b: dict, exclude: tuple[str, ...]) -> bool:
    name = (b.get("name") or "").lower()
    return (b.get("category") in exclude
            or name in EXCLUDE_NAMES
            or any(name.startswith(p) for p in EXCLUDE_NAME_PREFIXES))


def build(builtins: list[dict], exclude: tuple[str, ...]):
    kept = [b for b in builtins if not _excluded(b, exclude)]
    dropped = [b for b in builtins if _excluded(b, exclude)]
    documented, names_only = [], []
    documented_names: set[str] = set()
    for b in kept:
        if b.get("signature"):
            documented.append(b)
            documented_names.add(b["name"])
    # names-only = kept-without-signature, minus any name already documented
    # (some builtins surface under two category tags) and de-duplicated.
    seen: set[str] = set()
    for b in kept:
        n = b["name"]
        if b.get("signature") or n in documented_names or n in seen:
            continue
        seen.add(n)
        names_only.append(b)
    return kept, dropped, documented, names_only


def render_markdown(documented, names_only, dropped, exclude) -> str:
    from collections import defaultdict
    L: list[str] = []
    L.append("# Aether builtin reference\n")
    L.append(
        "The builtins below are the supported, non-graphics Aether surface. Use these "
        "exact names; never invent helpers. Pure builtins are called directly; builtins "
        "marked **effectful** must be called inside `fx { ... }` (and are rejected in "
        "`@pure` functions).\n"
    )

    # Queryability section -- the bridge for everything not fully documented.
    L.append("## Discovering builtins (query, do not guess)\n")
    L.append(
        "This list is not assumed exhaustive: the full builtin surface is queryable at "
        "runtime, and a host program can register more. Discover the exact name, "
        "signature, and `effectful` flag, then call it -- wrapping it in `fx` if "
        "effectful -- rather than guessing:\n"
    )
    L.append("- `builtins_json()` -> JSON array of every Aether-visible builtin name")
    L.append("- `builtins_json(true)` -> richer metadata per builtin "
             "(`signature`, `return_type`, `effectful`, `usage`, `category`)")
    L.append("- `builtin_info(name)` -> structured metadata for one builtin")
    L.append("- `aether --dump-ext-builtins` (CLI) -> the registered extended/custom builtins\n")
    L.append(
        "If a builtin you need is not fully documented here, query it first. Discovering "
        "a name is not permission to guess how it is called.\n"
    )

    # Fully documented builtins, grouped by category.
    by_cat: dict[str, list[dict]] = defaultdict(list)
    for b in documented:
        by_cat[b["category"]].append(b)
    L.append("## Documented builtins (signature + usage)\n")
    for cat in sorted(by_cat):
        L.append(f"### {cat}\n")
        L.append("| builtin | effectful | signature | usage |")
        L.append("|---|---|---|---|")
        for b in sorted(by_cat[cat], key=lambda x: x["name"]):
            eff = "yes" if b.get("effectful") else "no"
            sig = (b.get("signature") or "").replace("|", "\\|")
            use = (b.get("usage") or "").replace("|", "\\|")
            L.append(f"| `{b['name']}` | {eff} | `{sig}` | {use} |")
        L.append("")

    # Remaining builtins -- names only, grouped by category.
    if names_only:
        no_cat: dict[str, list[str]] = defaultdict(list)
        for b in names_only:
            no_cat[b["category"]].append(b["name"])
        L.append("## Additional builtin surface (names -- query `builtin_info` for signatures)\n")
        L.append(
            "These builtins exist and may be called, but are not fully documented here. "
            "Query `builtin_info(name)` (or `builtins_json(true)`) for the exact signature "
            "and `effectful` flag before use.\n"
        )
        for cat in sorted(no_cat):
            names = ", ".join(f"`{n}`" for n in sorted(no_cat[cat]))
            L.append(f"- **{cat}**: {names}")
        L.append("")

    # Brief provenance note. Deliberately counts-only: do NOT enumerate the
    # excluded names here -- this document trains a model, and listing the
    # hidden CRT/DOS/SDL surface would re-teach exactly what we excluded. The
    # full audit (names + reasons) lives in the JSON manifest, not the corpus.
    L.append("---\n")
    cat_dropped = [b for b in dropped if b.get("category") in exclude]
    name_dropped = [b for b in dropped if b.get("category") not in exclude]
    cats = ", ".join(sorted({b.get("category", "?") for b in cat_dropped}))
    L.append(
        f"*{len(documented) + len(names_only)} non-SDL Aether builtins. Excludes "
        f"{len(dropped)} hidden builtins ({len(cat_dropped)} in SDL/demo categories "
        f"({cats}); {len(name_dropped)} legacy CRT/DOS, VM introspection, and registry "
        f"junk). Generated by `tools/aether_export_builtins_reference.py`.*\n"
    )
    return "\n".join(L)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--aether-bin", type=pathlib.Path, required=True)
    ap.add_argument("--output-md", type=pathlib.Path)
    ap.add_argument("--output-json", type=pathlib.Path)
    ap.add_argument("--exclude-category", action="append", default=list(DEFAULT_EXCLUDE),
                    help="builtin category to exclude (repeatable); default graphics/3d/user")
    args = ap.parse_args()
    exclude = tuple(args.exclude_category)

    builtins = query_builtins(args.aether_bin)
    kept, dropped, documented, names_only = build(builtins, exclude)
    clean = documented + names_only  # deduped, signature-first

    print(f"total builtins: {len(builtins)}")
    print(f"excluded: {len(dropped)} "
          f"({len([b for b in dropped if b.get('category') in exclude])} by category "
          f"{sorted(set(exclude))}, "
          f"{len([b for b in dropped if b.get('category') not in exclude])} by name)")
    print(f"kept non-SDL (deduped): {len(clean)}  (documented w/ signature: {len(documented)}, "
          f"names-only: {len(names_only)})")

    if args.output_md:
        args.output_md.write_text(render_markdown(documented, names_only, dropped, exclude),
                                  encoding="utf-8")
        print(f"wrote markdown -> {args.output_md}")
    if args.output_json:
        manifest = {
            "excluded_categories": sorted(set(exclude)),
            "excluded_count": len(dropped),
            "kept_count": len(clean),
            "documented_count": len(documented),
            "builtins": [
                {k: b.get(k) for k in
                 ("name", "category", "effectful", "return_type", "signature", "usage")}
                for b in sorted(clean, key=lambda x: (x["category"], x["name"]))
            ],
        }
        args.output_json.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"wrote json -> {args.output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
