# Aether no-guide failure analysis (v8e broad run)

*Analysis artifact (overnight). For each recurring no-guide (`none`) failure across
the five v8e families, the actual generations were pulled and compiled, and each is
root-caused as **language defect**, **training gap**, **benchmark artifact**, or
**harness bug** — with an explicit "is the clean language fix non-kludgy?" verdict
where relevant. Nothing here is applied except the harness fix (§5), which is
tooling, not the language.*

Models: granite8b, qwen7b, qwen14b, mistralnemo12 (all bf16, compact corpus).
DeepSeek's eval is affected by the harness bug in §5 and is treated as preliminary.

## 0. The headline: most floors fail for *different* reasons per model

The tasks that fail `none` across families almost never fail the *same way*.
`release_board` fails 4/4 but via four distinct bugs; `config_validator`,
`toon_inline_extract`, `module_toon_report` similarly. **This is itself the most
important finding**: the "systematic floor" is mostly an artifact of a few
over-loaded tasks (see `aether_benchmark_gaps.md` §3), not a single language hole.
The genuinely language-shaped findings are §2.

## 1. Training gaps — wrong priors (fix the corpus, NOT the language)

These are cases where the model reached for an API that doesn't exist. The clean
answer is a corpus drill, **not** a language change — adding the invented forms
would be exactly the kludges to avoid.

- **`parse_json` instead of `toon_parse`** (granite, `toon_inline_extract`):
  `[SCOPE-001] identifier 'parse_json' not in scope`. The `toon_` prefix isn't
  internalized. A drill exists in the granite overlay but isn't fully taking —
  strengthen it / ensure the core corpus uses `toon_` uniformly.
- **4-arg `toon_get_int_or(node, key, default, outvar)`** (granite,
  `config_validator`): the model invents a C-style out-param to "get the value
  *and* test presence" in one call. `toon_get_*_or` is 3-arg and returns the
  value. **Verdict: do NOT add the 4-arg form** — out-params are anti-Aether.
  Drill: "use `toon_has_key` to test, `toon_get_*_or` to fetch."
- **Real-valued average** (everyone, `release_board`): models declare
  `average: Int` and do integer division → `82` not `82.25`; the 7B literally
  concatenated a hardcoded `".25"`; another wrote `sum*100/total`. **Verdict:
  primarily a training gap** — teach the int→real mean idiom. Worth a quick
  ergonomics check that `real(sum) / count` is actually clean in Aether (if the
  int→real conversion is awkward, *that* would be a language nit — see §2).
  **Confirmed empirically (wild-eval, 2026-06-17):** the generator of
  `aether_benchmark_gaps.md` §8 reproduced this with no prompting — Qwen-7B, asked
  for the mean of four integers, declared `let mean: Int = s / n;` and hand-rolled
  the fractional part (`50 * rem / n + adj`), emitting `mean = 39.26.000000` for an
  expected `39.50`. It was the *only* recurring miss on an otherwise-80%-clean
  novel set (16/20), so it is the top training-gap priority.

## 2. Language candidates — clean (non-kludgy) fixes worth considering

Each of these is a place where the *natural* program should arguably work, and the
fix extends an existing Aether principle rather than bolting on a special case.
**None applied** — flagged for your review.

- **`toon_get_*_or` crashes on TYPE mismatch instead of degrading.** (mistral,
  `release_board`): the field `id` is text `"R1"`; the model wrote
  `toon_get_int_or(node, "id", 0)` and got a hard runtime error
  `[AETH-RUNTIME-TOON-GET-INT-TYPE]` rather than the default `0`. The degradation
  fix (commit `c30d16260`) already made *missing keys / invalid handles* degrade
  to the default — a **type mismatch should degrade the same way**. **Verdict:
  clean and non-kludgy** — it's completing a principle we already committed to,
  not a new one. Caveat: it changes the *failure mode* (crash → wrong-but-running
  output), so it won't by itself flip `release_board`; it just stops a defensible
  program from crashing.
- **`<VOID_TYPE>` from a Bool-returning helper.** (qwen14b,
  `eligibility_bool_logic`): output is literally `guest 0: <VOID_TYPE>` — a pure
  helper that should return `Bool` was inferred as `Void`, so the value printed is
  the void sentinel. The other three families infer `Bool` fine on this task, so
  it's a specific shape the return-type inference mishandles. **Verdict: needs a
  source-level look** (TODO: read the 14B's helper); if it's a real inference gap,
  the fix is clean. If the model wrote a genuinely void-returning function, it's a
  model error. Worth resolving because a model printing `<VOID_TYPE>` instead of
  erroring is a bad failure mode regardless.
  **Update (2026-06-17):** a *second*, language-level source of `<VOID_TYPE>` was
  found and fixed (commit `4d475cda5`) — negative literals in `Int[]` array
  literals (`let xs: Int[] = [-5, ...]`) printed `<VOID_TYPE>` at index 0 (or
  leaked a `Real` at index >0, e.g. `clamp(xs[1],0,100) = 0.000000`), because
  `evaluateCompileTimeValue`'s unary-minus case matched only `TYPE_INTEGER` while
  REA integer literals are `TYPE_INT64`. Lesson: `<VOID_TYPE>` is **not** a
  reliable model-error signal — it can be a genuine compiler bug. (The Bool-helper
  case above is still plausibly model error, but should be re-confirmed against the
  actual 14B source rather than assumed.)
- **`default` is a reserved token.** (granite, `module_toon_report`):
  `[SYN-001] Unexpected token DEFAULT 'default'` — the model used `default` as a
  field/identifier. **Verdict: investigate** — if `default` is reserved for a
  feature Aether doesn't expose to users (or doesn't have), freeing it as a valid
  identifier is a clean win; if it's load-bearing (switch/case), leave it and
  drill the corpus.

## 3. Benchmark artifacts (fix the benchmark, see `aether_benchmark_gaps.md`)

- **`release_board` is over-loaded** (6+ sub-skills) and **ambiguously specified**
  ("normalize into 0..100" → two models scaled ×10). It fails universally but
  diagnostically tells us nothing. Split + clarify.
- **`module_toon_report`** similar (module + TOON + formatting); fails for mixed
  reasons (string-literal syntax in mistral, the `default` token in granite).

## 4. Model-idiosyncratic / translate.c rewrite-layer stress

Granite (the weakest family) hits more `[SYN-001] Aether function rewrite error`
and token-level parse errors than the others (e.g. `toon_type_probe` line-184
function-rewrite error). These point at the **bootstrap rewrite layer** (the same
class as the one-liner wart) choking on shapes Granite emits that the Qwen/Mistral
families don't. Lower priority than §2, but a cluster worth a pass once the §2
items are resolved — each is a potential rewrite-layer hardening like the
one-liner fix.

## 5. Harness bug (fixed) — UTF-8 decode crash

DeepSeek's eval produced 21+ `returncode=-1` cases from
`'utf-8' codec can't decode byte 0xc4` — the bench harness reads compiled-program
stdout with strict UTF-8 and **crashes** on any non-UTF-8 byte, scoring the task a
failure. This undercounts any model whose program emits a stray byte (DeepSeek hit
it; others could). **Fix applied:** decode subprocess output with
`errors="replace"` so a stray byte yields a (mismatching) string instead of a
crash. DeepSeek's `none` from this run is therefore a **lower bound**; a re-eval
with the fixed harness gives the true number (teed up, not run — it needs the GPU).

## 6. Prioritized recommendations

**Language (clean fixes, for your review — not applied):**
1. Extend `toon_get_*_or` graceful-degradation to **type mismatch** (§2) —
   completes the degradation principle.
2. Resolve the **`<VOID_TYPE>` Bool-inference** case (§2) — read the 14B source;
   fix if it's an inference gap.
3. Investigate freeing **`default`** as an identifier (§2).

**Training (corpus):**
4. Reinforce `toon_` prefix + "no 4-arg out-param" drills across families (§1).
5. Add the **real-mean idiom** to the corpus (§1).

**Benchmark (see gaps doc):**
6. Split the kitchen-sink tasks; fix the ambiguous "normalize" spec; trim TOON;
   add the holes (par, effect-boundary negative test, clamp, real-mean, recursion).

**Tooling:**
7. ✅ Harness UTF-8 decode hardening (done). Consider a re-eval of DeepSeek.
