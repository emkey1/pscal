# Corpus re-verification vs the 2026-07-01 hardening batch

## POST-FIX STATUS (2026-07-01, second pass)

All corpus fixes applied and the full driver re-run on `/Users/mke/PBuild/build/bin/aether`
(20260701.1525_DEV, language **2026-07-01-8** — includes the R1 `@pure`+`export fn`
direct-compile fix). Fresh per-unit results in the session scratchpad `results/`.

| Set | Units | Pass | Fail |
|---|---|---|---|
| Corpus candidates with expected stdout (union, +2 new) | 661 | 660 | 1 (`35_capability_probes`, environment-dependent: this build links the openai ext-builtin, pinned stdout assumes a non-AI build) |
| Module-file candidates (standalone compile, no stdout) | 14 | 14 | 0 (R1 fixed in -8) |
| Instruction-pair solutions | 17 | 17 | 0 |
| Repair-pair fixed sources | 61 | 61 | 0 |
| Repair-pair broken sources (must fail) | 61 | 59 still fail | 2 "compile" = the advisory-by-design `repair_float_formatting` (default+qwen25), kept intentionally |

Remaining pinned-diagnostic drift: **0** (every non-advisory broken pair's `diagnostic`
now pins the exact current `[CODE] message` emission).

What changed in this pass:
- FX-001 candidates 62/63/67/69/111: `toon_parse_file` wrapped in `fx { ... }` (110's pattern); stdout unchanged, all pass; manifests re-sha'd.
- `module_dual_helpers` instruction pair: quoted `use "..."` + `println(label, " ", total)`; now prints `Result: 15` exactly.
- Stale broken pairs (section e): deleted `repair_type_for_toon_handle`, `repair_println_string_concat`, `repair_method_self_parameter`, `repair_toon_guarded_intermediate` (default+qwen25) and `repair_granite_toon_prefix_parse_root` (granite) — their broken forms are now accepted language. Replaced `repair_new_struct_init` (was INVERTED) with `repair_no_constructor_method` (`fn new()` ctor method, still rejected → `new Point { x: 3, y: 4 }`). Replaced `repair_invented_import`'s broken half with one that *uses* a symbol from the missing module (fails `[SCOPE-001]`), teaching the still-true rule that a missing import's symbols do not resolve. `repair_float_formatting` left as-is (advisory).
- Diagnostic drift (section f): all still-failing broken pairs re-pinned to actual emissions, `[CODE]` + message form.
- New coverage drills (default+qwen25 overlays, mirror preserved): `repair_field_default_constant` (FIELD-003), `repair_bare_ret_value` (FLOW-002), `repair_par_direct_calls` (PAR-002), `repair_par_shared_record` (PAR-001), `repair_tuple_recursion_record` (TUP-001). New positive candidates: `314_field_defaults_partial_init` (constant defaults + `new T { field: value }` partial init), `315_par_per_branch_records` (first `par` usage in the corpus; deterministic).
- `@cost` entries 11/41: comments reframed as advisory/non-binding (syntax untouched).
- Builtin reference regenerated cleanly from the -8 binary (`tools/aether_export_builtins_reference.py`: 503 total, 262 kept non-SDL, 110 documented). Note the reference is generated live at export time; there is no checked-in copy.
- Dataset versioning: there is no checked-in corpus version field — the stamp is applied at export time by `tools/aether_specialization_prepare_assets.py` (`--version`, default today-1) into `aether_training_mix.json`, alongside `aether_version` from `components/aether/VERSION`. A full export smoke run to a scratch dir with `--version 2026-07-01-1` succeeded end-to-end (408 instruction records, 27 repair records, 0 verification failures). Per-family training sets must be re-exported (`--repair-manifest seed_repair_pairs.<family>.json`) before the next training run; the historical `out*/` dirs were left untouched.
- `python3 tools/aether_specialization_validate_corpus.py --strict` passes.

Original first-pass report follows (its failure lists are now historical).

---

- **Binary:** `/Users/mke/PBuild/build/bin/aether` (20260701.1525_DEV, language 2026-07-01-7)
- **Date:** 2026-07-01, local compile+run only (`--no-cache`, cwd seeded with `fixtures/` + module files, 20s timeout per unit)
- **Scope:** union of all four candidate manifests (main + 1x/2x/3x; 3x extras resolved from `corpus_candidates_archive_old/`), `seed_instruction_pairs.json`, and all three `seed_repair_pairs.*.json` overlays (broken AND fixed halves)
- **Machine-readable results:** scratchpad `corpus_reverify_2026-07-01.json` + per-unit `results/*.json` (session scratchpad `/private/tmp/claude-501/-Users-mke-PBuild/ab32b92a-0ee3-4ec1-b3b3-eb60c822147d/scratchpad/`)
- **Nothing was modified or committed.** Verification only.

## Headline counts

| Set | Units | Pass | Fail |
|---|---|---|---|
| Corpus candidates with expected stdout (union, deduped) | 659 | 653 | 6 (5 compile, 1 stdout) |
| Module-file candidates (no expected stdout, standalone compile) | 14 | 2 | 12 (all one ANN-001 issue, see R1) |
| Instruction-pair solutions | 17 | 16 | 1 (stdout) |
| Repair-pair fixed sources | 60 | 60 | 0 |
| Repair-pair broken sources ("must still fail") | 60 | 45 still fail | **15 now compile (stale)** |

No hangs, no timeouts, no signal crashes anywhere. All 255 archived (3x-manifest) entries pass. All failures are in the main manifest / seed files.

Note: pair verification requires module files (`bench_consts`, `bench_math`, `math_utils`, `text_utils`, `severity_rules`, `severity_labels`, `exact_exports`, `bench_support`, `color_rules`) plus `support_jobs.json` in cwd; these were synthesized from the corresponding corpus module candidates. `color_rules` has **no canonical source anywhere in the repo** (had to be invented) — that is itself a fixture gap.

## (a) Compile failures — all `[FX-001]`, all NEW-BINARY-LEGITIMATE

5 candidates call `toon_parse_file(...)` outside `fx`. 2026-07-01-5 intentionally classified `toon_parse_file` (YyjsonReadFile) as effectful, so these now die with e.g.:

```
corpus_candidates/62_toon_parse_file_simple:15: [FX-001] Aether effect error:
call to 'toon_parse_file' requires an fx block.
```

Entries: `62_toon_parse_file_simple`, `63_toon_safe_nested_codes_file`, `67_config_validator_nested`, `69_release_board_file`, `111_support_module_rollup_exact`. Fix: wrap the call in `fx { ... }` (110_jobs_summary_file_exact already does and passes). These currently teach the exact escape the fence just closed.

## (b) Stdout mismatches — 2, both CORPUS-BUG (not compiler)

1. `35_capability_probes`: pinned `ai: missing`, binary prints `ai: available`. Metadata already flags `environment_dependent: true` — this build links the openai ext-builtin. Pinned stdout is only valid for non-AI builds.
2. `seed_instruction_pairs.json` / `module_dual_helpers`: expected `Result: 15`, program prints `Result:15`. The repo-canonical `96_text_utils` returns `"Result:"` and `println(label, total)` inserts no separator; solution needs `println(label, " ", total)` (or the module a trailing space). Solution also uses unquoted `use math_utils;` where MOD-002 canon is quoted.

## (c) Hangs/crashes

None.

## (d) POSSIBLE-COMPILER-REGRESSION — flagging loudly

**R1: `@pure` + `export fn` inside `mod` rejected on DIRECT compile, accepted via import.**

```aether
mod M {
    @pure
    export fn f(n: Int) -> Int {
        ret n + 1;
    }
}
```

`aether --no-cache mod_pure.aeth` → `mod_pure.aeth:2: [ANN-001] Aether contract error: @pure must annotate the next function declaration.` (rc=1). The SAME file imported via `use "..."` compiles and its consumers all pass. The guide explicitly canonizes "place `@pure` on the line above `export fn`". Hits 12 of the 14 module-file candidates (all that use `@pure`; `102_bench_consts`/`143_tax_rates` have no `@pure` and standalone-compile fine). Likely the direct-compile path's annotation binder doesn't skip `export`. Minimal repro kept at scratchpad `repro/mod_pure.aeth`. Low blast radius for training (modules are never run standalone) but it is a natural, guide-conformant program rejected on one path — worth a compiler look.

## (e) Stale broken repair pairs — 15 hits, 8 unique (default overlay mirrors qwen25)

These "broken" sources now compile and run, so the drill no longer demonstrates a failure:

| Pair id (overlays) | Broken idiom | Verdict |
|---|---|---|
| `repair_new_struct_init` (default+qwen25) | `new Point { x: 3, y: 4 }` | **INVERTED** — this is now the guide-recommended form (2026-07-01-4 FIELD-003 batch). The pair actively teaches against current canon. Drop/rewrite. |
| `repair_type_for_toon_handle` (d+q) | untyped `let doc = toon_parse(...)` | Legalized (inference work, 2026-06-30-3). Stale. |
| `repair_invented_import` (d+q) | `use "helpers";` (module absent) | Missing imports are now silently ignored until a symbol is used (documented in guide: "a missing import's symbols do not resolve"). Pinned "unable to open module" never fires. Stale. |
| `repair_println_string_concat` (d+q) | `println("score = " + score)` | `Text + Int` now coerces; broken output equals the fixed pair's expected output. Stale. |
| `repair_method_self_parameter` (d+q) | explicit `self: Counter` method param | Compiler tolerates it (runs correctly) though the guide says never. Guide/compiler divergence; pinned diagnostic never fires. |
| `repair_toon_guarded_intermediate` (d+q) | `toon_get_text_or(toon_key(item,"meta"),...)` on missing intermediate | Now returns the fallback instead of erroring; broken output equals fixed expected. Stale. |
| `repair_float_formatting` (d+q) | verbose default Real print | Advisory/style pair — broken compiles by design (likely always did); polarity check flags it definitionally. Keep, but it never had a hard diagnostic. |
| `repair_granite_toon_prefix_parse_root` (granite) | `parse_json()` / `root_node()` | These are now registered builtin ALIASES for `toon_parse`/`toon_root` (`ast_prepasses.c:3235`). Stale — and note the guide still says all TOON builtins use the `toon_` prefix. |

## (f) Diagnostic-text drift in still-failing broken pairs — 29 of 45

Correct polarity (still fail), but the pinned `diagnostic` field no longer matches what the compiler emits — both format (`path:line: [CODE] ...` vs old raw text) and often the message/code itself (e.g. pinned "Aether contract rewrite error: @pre must annotate..." now surfaces as `[SYN-001] unexpected token in block`; pinned "Runtime Error: Undefined global variable 'PI'" is now a compile-time `[SYN-001]`; pinned availability/prefix TOON errors are now generic `[SCOPE-001] identifier ... not in scope`). If the repair SFT prompt embeds the diagnostic verbatim, 29/60 pair-halves train on compiler text that no longer exists. Full list in the JSON (`diag_drift`).

## (g) Coverage gaps vs the new surface (inventory only)

- **FIELD-003 / constant field defaults** (`value: Int = 0` in a type body): zero corpus candidates use them (main + oneliner swept).
- **FLOW-002** (empty `ret;` in non-Void function): untaught anywhere.
- **PAR-001/PAR-002**: zero `par { }` blocks and zero `task_spawn` in the entire corpus — the concurrency surface is untaught.
- **TUP-001 tuple recursion**: 8+ tuple candidates, none demonstrates "recursive helpers must return a record, not a tuple".
- **Scoped-binding idioms**: no candidate exercises same-name locals of different types in different functions (the teachable case the function-scoped tables just made correct).
- **@cost non-binding framing**: 2 entries use `@cost` (`11_cost_annotation`, `41_contract_quote`) but neither frames it as advisory/non-binding.
- **fx with `{` on the next line** (newly legal per 2026-07-01-5): not represented.
- **`color_rules` module** (needed by `repair_use_module_direct_call`): no canonical file in the repo.

## Alias/string-literal check

The flagged expected-stdout corruption pattern (`delay(`/`writeln(` in pinned outputs where source says `sleep(`/`println(`) does not occur: no candidate or pair expected_stdout embeds it, and no mismatch of that shape appeared in the run.
