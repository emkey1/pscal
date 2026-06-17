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
