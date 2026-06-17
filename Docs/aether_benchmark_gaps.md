# Aether benchmark: coverage gaps & v2 proposal

*Analysis artifact (overnight, post-v8e broad run). Audience: maintainers deciding
what the next benchmark version should test. Nothing here is applied — `tasks.json`
is untouched; this is a proposal.*

The current suite is **29 tasks** (`Tests/aether_doc_bench/tasks.json`), each a
natural-language spec graded by exact stdout under three doc variants
(full/small/none). This doc inventories what it covers, what it over-weights, and
what it misses — informed by the five-family v8e results.

## 1. Coverage inventory (by mechanism tag)

| Area | Tasks | Count |
|---|---|---|
| **TOON** (`toon`) | toon_inline_extract, toon_parse_file_simple, toon_type_probe, toon_safe_nested_codes, toon_deep_nested, config_validator, release_board, module_toon_report | **8 (28%)** |
| Types & methods | type_init_point, inventory_status, two_type_methods | 3 |
| Real formatting | real_format_circle, percentage_format (+ release_board) | 2–3 |
| Modules | module_function_import, module_const_import, module_toon_report | 3 |
| Loops | range_loop_squares, loop_until_threshold, nested_multiply_grid | 3 |
| Tuples | tuple_destructure, tuple_order_post | 2 |
| Contracts (`@pre/@post`) | contract_normalize, tuple_order_post | 2 |
| Dynamic arrays | dynamic_array_rollup, array_minmax | 2 |
| Bool logic | eligibility_bool_logic, string_checks | 2 |
| Branching / inline-if | classify_scores, inline_if_average | 2 |
| Output / const | hello_fx, hello_const_name | 2 |

Every task uses `fx` for output and many use `@pure` helpers, so those are
exercised pervasively (if never tested *adversarially*).

## 2. Over-representation: TOON

**TOON is 28% of the suite and dominates the failures.** Of the recurring
no-guide failures across the five families, the large majority are `toon_*`
tasks, and the three hardest tasks in the whole suite (`release_board`,
`config_validator`, `module_toon_report`) are all TOON-plus-other-stuff.

Consequence: the benchmark's headline number is disproportionately a **TOON-access
score**, not an Aether-fluency score. A model that nails control flow, types,
contracts, and modules but is shaky on nested TOON extraction looks worse than its
actual Aether fluency. Recommend **trimming TOON to ~4–5 focused tasks** and
reallocating the freed slots to the holes below.

## 3. The "kitchen-sink" tasks dilute signal

`release_board` (and to a lesser degree `config_validator`, `module_toon_report`)
each bundle **6+ independent sub-skills**: nested TOON parse → per-row normalize →
classify → count by category → real-valued average → boolean derivation →
formatted output. Because grading is exact-stdout, **any single slip fails the
whole task**, and the five families fail it for *different* reasons (×10
normalize, integer average, a type-mismatch crash, a mangled average formula).

That makes these tasks **poor discriminators and poor design probes** — you can't
read "release_board failed" as evidence about any one skill. Recommend **splitting
each kitchen-sink task into 2–3 single-skill tasks** (e.g. a dedicated
real-average task, a dedicated normalize-and-clamp task, a dedicated
classify-by-threshold task). Same coverage, far higher diagnostic value, and the
`none`-score stops being gated on getting six things right at once.

## 4. Spec ambiguities that manufacture failures

`release_board` says *"Normalize scores into 0..100."* The sample scores are
already in 0..100, so the intended op is a clamp — but two of five models read
"normalize into 0..100" as a scaling and wrote `score*100/10` (→ ×10, `920`
instead of `92`). That's not a model defect; it's an **under-specified prompt**.
A benchmark task should have exactly one reasonable reading. Audit all prompts for
verbs like "normalize", "scale", "round" that imply a transform without pinning
the input domain.

## 5. Holes — untested language surface

Ranked by how load-bearing the feature is vs. how untested it is:

1. **Concurrency / `par` / `task_spawn`/`task_wait`** — **0 tasks.** Aether
   advertises a concurrency surface; nothing exercises it. Highest-value add if
   `par` is meant to be first-class.
2. **Effect-boundary enforcement** — **0 tasks.** Every task *uses* `fx`, but
   none tests that an effectful call *outside* `fx` (or inside `@pure`) is
   **rejected**. The effect system's whole point is unverified. Add a
   negative/should-fail task.
3. **Real arithmetic (not just formatting)** — under-tested. The real-valued
   **average** is a repeated failure (everyone botches `82.25`), yet no focused
   task isolates "compute a real mean from integer data." Add one.
4. **`clamp` / `min` / `max`** — **0 tasks.** Newly added builtins, untested;
   and a clamp task would directly target the normalize-bug above.
5. **Negative / invariant tests** — **0 tasks.** Aether deliberately rejects
   `1..=3`, `new T{}`, 4-arg `toon_get_*_or`, one-liner `type`. None of these
   "the compiler should reject X" invariants is pinned by a test, so a regression
   that *accepts* them would pass silently. Add a small negative-test tier.
6. **Recursion** — **0 tasks.** No self-referential function (factorial-style).
7. **Error / `fail` paths** — **0 tasks.** No task exercises a failure effect or
   error propagation.
8. **String manipulation** — thin (one `string_checks`). No substring/concat/find.

## 6. Difficulty distribution

Bimodal: two trivial tasks (`hello_*`) and a cluster of very hard TOON kitchen-sink
tasks, with a thin middle. The trivial tasks add little signal (every model passes
`none`); the kitchen-sink tasks add noisy signal (failure for mixed reasons). A
healthier distribution is a **graded ramp** — more single-skill mid-difficulty
tasks — which also makes the `none` score move more smoothly as the language/corpus
improve, instead of lurching when one six-step task flips.

## 7. Proposed v2 shape (same ~29-task budget)

- **Trim** TOON 8 → ~5 (keep: inline_extract, parse_file_simple, type_probe,
  one nested-guard, one deep-nested; drop/merge the rest).
- **Split** `release_board` and `module_toon_report` into single-skill tasks
  (real-average, normalize+clamp, classify-by-threshold, count-by-category).
- **Add** the holes: one `par`, one effect-boundary *negative* test, one
  real-mean, one `clamp`, a small **negative-invariant tier** (3–4 should-reject
  cases), one recursion.
- **Audit** every prompt for single-reading specs (start with "normalize").
- Keep the mechanism tags; add a `should_fail` flag for the negative tier so the
  scorer expects a compile/parse rejection rather than stdout.

Net: the headline `none` number becomes a broader, less TOON-skewed, more
diagnostic measure of Aether fluency — and the negative tier turns the language's
deliberate rejections (a real part of the design) into things the benchmark
actually defends.

**Status — a first verified increment is drafted** in
`Tests/aether_doc_bench/tasks_v2_additions.json`: five single-skill splits of
`release_board` (`real_mean_format`, `clamp_scores`, `classify_threshold`,
`count_by_category`) plus the recursion hole (`recursion_factorial`), and a
two-task **negative-invariant tier** (`reject_inclusive_range` for `..=`,
`effect_boundary_reject` for an effectful call outside `fx`). Each was compiled
and run against `build/bin/aether` — the five positives match their
`expected_stdout` exactly; the two negatives are rejected with the expected error
codes (`SYN-001`, `FX-001`). The negative tier needs a `should_fail` scorer path
(compile, pass iff rejected with `expected_error_code`).

## 8. Authoring v2 with generation assistance (methodology)

*§5 and §7 above are hand-reasoned from five models' failures — i.e. the author's
best guess at what's missing. This section proposes using random task generation to
put that guess on an empirical footing, and is deliberate about where generation
helps and where it actively hurts.*

**Authoring ≠ evaluation — and the difference dissolves the oracle problem.** A
generator used to *score* models must auto-grade thousands of throwaway tasks, so a
flaky or ambiguous oracle (no trusted expected output) poisons the result — this is
the central obstacle to generation-based *eval*. A generator used to *author*
instead produces a large candidate pool that is **curated down to ~20–30 tasks**,
each of whose expected output is hand-verified exactly once and then frozen. That
one-time verification amortizes over every future eval run, and a maintainer in the
loop rejects the infeasible and the ambiguous before they ever enter the suite. The
oracle problem is therefore nearly absent in generation-based *authoring*.

**What generation uniquely contributes — the holes the author cannot see.** The §5
hole list (par, recursion, strings, clamp, …) is the *obvious* set: an author can
only enumerate the features they already know are under-tested. What a broad
generator adds is the **combinatorial interaction space** — e.g. "loop +
tuple-return + real-format *together* fails although each works in isolation." No
hand-authored suite probes feature interactions, because authors test one mechanism
at a time. Running a candidate pool against the v8e models thus does something §5
cannot: it **empirically ratifies or corrects the hand-authored proposal**. If
generation only re-surfaces holes already in §5, that is confidence; if it surfaces
others, §5 was incomplete. Two secondary wins: generation mass-produces the
**mid-difficulty single-skill tasks** §6 asks for (the graded ramp), and it
**counteracts author bias** — a hand author writes tasks in the same idioms the
corpus already favors, so the suite inherits the corpus's blind spots; a
differently-sourced generator writes shapes the author wouldn't.

**What generation cannot do — the intent-driven precision work.** Several v2 items
in §7 require *knowing what the language deliberately rejects*, which a random
generator has no representation of:

- the **negative-invariant tier** (`1..=3`, `new T{}`, 4-arg `toon_get_*_or`,
  one-liner `type` must be *rejected*) — a generator cannot author a "this should
  fail to compile" task because it has no model of intent;
- the **effect-boundary should-fail** test (an effectful call outside `fx` or
  inside `@pure` must be rejected) — same reason;
- **splitting the kitchen-sink tasks** (`release_board`, `module_toon_report`) —
  that is surgical decomposition of *existing* tasks, not generation;
- the **single-reading spec audit** (§4) — that is editorial review; generation
  *creates* ambiguity (`release_board`'s "normalize" is exactly the phrasing a
  generator emits), it does not remove it.

These stay hand-authored from the language design. v2 is therefore necessarily a
**hybrid**: hand-authored targeted tasks for the intent-driven holes, plus a
curated generated subset for coverage breadth and the difficulty ramp.

**The contamination constraint — stricter for a benchmark than for eval.** If the
generator is the same model family being trained, or draws on the corpus's idioms,
v2 becomes a mirror of the training distribution and silently reintroduces the
overfit it was meant to detect. The discipline: an **Aether-naïve frontier model**
generates **natural-language specs only** — a problem statement, sample input, and
intended behavior in prose, *never Aether code*. The task stays language-agnostic;
the only Aether-aware step is the trusted Python reference that produces the
expected output, and that is the curator's job, not the generator's. This keeps v2
a genuine held-out instrument rather than a second copy of the corpus.

**The pipeline (run once, to produce a frozen v2):**

1. **Spec generation** — an Aether-naïve model emits NL specs sampling the §1
   mechanism inventory at a target difficulty (skewed toward the thin middle of §6).
2. **Feasibility + oracle pass** — for each candidate, write a Python reference and
   run it for the expected output; discard anything not expressible in Aether's
   surface.
3. **Unambiguity gate** — require two independently-written references to agree on
   the output; disagreement means the spec has more than one reading (the §4 failure
   mode) and it is dropped.
4. **Difficulty + dedup filter** — bin by mechanism tag, drop near-duplicates of
   each other and of v1, fill the graded ramp.
5. **Curation** — the maintainer promotes survivors, hand-verifies each oracle,
   tags mechanisms.
6. **Hand-authored additions** — the negative-invariant tier, the effect-boundary
   test, and any §5 holes the generator missed are written directly.
7. **Freeze** — the union is frozen as v2; it is *not* regenerated per run
   (regeneration would make scores incomparable across time).

**Same generator, two products.** The only thing separating a benchmark from a
wild-eval probe is whether the output is frozen. Curate-and-freeze → a stable,
comparable benchmark (this section). Regenerate-every-run → the continuous
generalization probe. And the candidates that *don't* make the cut are not waste:
those that break trained models become the wild-eval finding log, and those that
break the *compiler* are candidate language bugs (the same class as
`aether_failure_analysis.md` §2).

**Cheap first step.** Build the spec-generator + Python-oracle pipeline at small
scale, generate ~50 candidates, and run them against the v8e models. CPU-only — no
contention with training. The output is the first empirical check on whether §5/§7
are right: it either ratifies the hand-written v2 proposal or rewrites it with
evidence.
