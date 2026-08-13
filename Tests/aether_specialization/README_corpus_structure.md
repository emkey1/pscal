# Aether SFT corpus: core + per-family overlays

The corpus is split into a model-agnostic **core** used by every model, plus
**per-family overlays** of remedial repair drills.

## Core (used by everything)
- `corpus_candidates/` + `corpus_candidates_manifest.json` — verified positive
  examples teaching correct Aether syntax/idioms. Universal; every model trains
  on these.

## Per-family overlays
Without a reference guide, different model families fall back to *different*
wrong priors, so a corpus tuned to one family under-serves another. Empirically
(no-guide /29, fixed compiler): Qwen2.5-Coder 24 (corpus tuned to it) > Qwen3-4B
23 > Granite-8B 20 — score falls with distance from the tuned family. Each
overlay is a set of `broken -> fixed` repair pairs authored by **probing that
family's actual none-failures**:
- `seed_repair_pairs.qwen25.json` — Qwen2.5-Coder (reverts: `@fx`, `1..=3`,
  `new T{}`, `has_toon_parser`, `ToonKind*`)
- `seed_repair_pairs.granite.json` — IBM Granite (reverts: 4-arg
  `toon_get_text_or(node,k1,k2,default)`, `@post result[0]` bracket-index)
- `seed_repair_pairs.json` — default (currently mirrors qwen25)

## Build a family's training set
```
python3 tools/aether_specialization_prepare_assets.py \
  --output-dir <out> --repair-manifest seed_repair_pairs.<family>.json
```
=> core positives + that family's overlay (the other families' overlays are
excluded — they target reverts this family doesn't make).

## Adding a new family
1. Train on core (+ closest overlay or none), serve, eval `none`.
2. `python3 tools/none_fail_detail.py <eval>.json none` to read its failing
   generations and identify its specific wrong priors.
3. Author `broken -> fixed` pairs (verify each compiles) -> `seed_repair_pairs.<family>.json`.
4. Rebuild + retrain on core + the new overlay.
