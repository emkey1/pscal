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
  a `fn new()` constructor method, `has_toon_parser`, `ToonKind*`; note that
  `new T { field: value }` is valid Aether, not a revert)
- `seed_repair_pairs.granite.json` — IBM Granite (reverts: 4-arg
  `toon_get_text_or(node,k1,k2,default)`, `@post result[0]` bracket-index)
- `seed_repair_pairs.json` — default (currently mirrors qwen25)

## Drill kinds and the polarity gate
Every drill declares `kind`:
- `repair`: `broken_source` is rejected by the compiler. Its stored
  `diagnostic` starts with the code the compiler emits first (`[FX-001] ...`),
  or is the uncoded first line for a runtime error.
- `behavioral`: `broken_source` compiles and runs but prints the wrong thing
  (`repair_float_formatting`, `repair_println_placeholders`,
  `repair_array_inplace_mutation`). If the compiler warns about it, the stored
  diagnostic cites that warning (`[ARR-001] ...`).

`tools/aether_specialization_build_dataset.py` runs every `broken_source` on
every build and fails on a drill whose polarity flipped ("drill obsolete": a
repair drill that now compiles, a behavioral drill that now prints the
expected output or no longer runs) or whose first emitted code changed ("code
drift X->Y"). A drill that "repairs" valid code into longer code teaches the
opposite of what the language now accepts, so it never trains. The SFT prompt
carries the compiler's live stderr (normalised to `sample.aether:N:`); the
stored text is kept for review diffs.

Legalizing a construct can invert drills, so in the gitlink-bump commit of
every release (and with any CHANGELOG entry that accepts something new) run:
```
python3 tools/aether_specialization_refresh_drills.py --aether-bin <aether>
```
It exits 1 on polarity failures and code drift. `--write` re-pins stored texts
whose code still matches; `--write --accept-code-drift` also re-pins drills
whose code changed, after you have checked the new code is the right one. A
drill with a flipped polarity is deleted or repointed by hand at a construct
that is still rejected, and its natural form moves to
`seed_instruction_pairs.json`.

## Build a family's training set
```
python3 tools/aether_specialization_prepare_assets.py \
  --output-dir <out> --repair-manifest seed_repair_pairs.<family>.json
```
=> core positives + that family's overlay (the other families' overlays are
excluded — they target reverts this family doesn't make).

## Adding a new family
1. Train on core (+ closest overlay or none), serve, eval `none`.
2. `python3 tools/aether_failure_histogram.py <eval>.json --doc none --by-construct --examples 3`
   to read its failing generations, bucketed by the construct each one
   reached for, and identify its specific wrong priors.
3. Author `broken -> fixed` pairs with a `kind` (verify each fixed source
   prints its expected stdout and each broken source still fails the way the
   drill says) -> `seed_repair_pairs.<family>.json`.
4. Rebuild + retrain on core + the new overlay.
