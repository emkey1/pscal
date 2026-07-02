# Benchmark failure mining — 2026-07-01

Historical failure logs mined against the CURRENT compiler (`/Users/mke/PBuild/build/bin/aether`,
`Aether Compiler Version: 20260701.1525_DEV`, language version **2026-07-01-8**). Every
historical failing generation was re-compiled/re-run against the current binary (with task
fixtures + expected stdout from `Tests/aether_doc_bench/tasks_{v2_pos,hard,cs}.json`) before
classification, so already-fixed failure classes are filtered out rather than re-taught.

Method + intermediate artifacts: scratchpad scripts `extract_failures.py` / `retest.py`
(session scratchpad); classifications below are per-record, deduped by generation hash.

## 1. Logs mined

100 result files, most recent batch per (model, taskset):

| Source | Files | Batches | Binary stamps |
|---|---|---|---|
| `Tests/aether_doc_bench/out/*_{v2,large,cs}_2026-06-28-1.json` (32 model boards x 3 tasksets, incl. trained `*-cs-aug2*` / `q36-*` boards) | 90 | 2026-06-28-1 | `2026-06-28-1` … `2026-06-30-4` |
| `*_2026-06-27-2.json` (models with no newer run: lmstudio qwen3-coder-30b etc.) | 6 | 2026-06-27-2 | `2026-06-27-2/3` |
| `idea_miner_2026-06-29.json`, `idea_miner_2026-06-29_sweep3.json`, `idea_miner_2026-07-01_pass2.json`, `idea_miner_2026-07-01_glm.json` | 4 | — | `2026-06-27-3` … `2026-07-01-1` |

Tasksets: `v2` -> `tasks_v2_pos.json`, `large` -> `tasks_hard.json`, `cs` -> `tasks_cs.json`.

## 2. Classification table

817 final-state failures (post-repair-loop) extracted; all with recoverable source re-run on 2026-07-01-8:

| Historical kind | n | Retest vs current binary |
|---|---|---|
| compile/runtime failure | 587 | 584 STILL-FAIL, 3 now run (wrong stdout) |
| wrong stdout (ran, mismatched) | 139 | 137 still wrong, 2 now compile-fail (stricter parse) |
| generation failure (no source) | 65 | HARNESS ARTIFACT — not retestable |
| idea-miner unfixed | 26 | 25 STILL-FAIL, 1 now runs |

Headline: **essentially nothing in the 06-27/06-28 failure logs was a since-fixed compiler bug** —
the failures are model priors, so the mining below is real corpus signal, not stale noise.
The one big FIXED-SINCE class is diagnostics: **188 of 190 historical silent rc=1 compile
failures (empty stdout+stderr) now emit proper `[SYN-001]` messages** on the current binary.

### Retest-verified language facts backing the drills (probe suite, current binary)

- Array args are **value copies**: `fn poke(xs: Int[]) { xs[0]=99; }` leaves caller unchanged (runs, prints old value). Returning the mutated array (`ret xs;`) works.
- `xs + ys` (two arrays) **compiles then runtime-errors** ("Operands must be numbers… Got ARRAY and ARRAY"); `xs + [v]` append is fine.
- Strings are **1-based**: `s[1]` ok, `s[0]` runtime error; `pos`/`copy` 1-based.
- `Int[][]` 2D dynamic arrays **work** (build rows by append; `dp[i][j] = v` ok).
- `else if` chains OK as statements, **rejected inside if-expressions**; nested `else { if … }` works.
- `loop i in 0..n+1` (expression bounds) works; `..=` does not.
- `new Int[](5)` compiles then VM-errors; fixed-size types (`Int[10]`, `[0; n]`) parse-error.
- Tuple element arrays `(Int, Bool)[]` parse-error; `abs(x)` ok, `(x).abs()` not; nested `fn` in a body not supported.

## 3. Ranked NEW corpus drill proposals (WRONG-PRIOR, uncovered)

Coverage checked against `seed_repair_pairs.json` (28 pairs: covers `..=` ranges, `return`->`ret`,
fx-println, float formatting, contract placement, toon doc-vs-root, marker leak, …) and
`corpus_candidates/` (420 units; 307_sorting/309_dp1d/310_dp2d/312_strings exist but are
**positive-only** — none demonstrate the wrong form). All proposals below are repair-style
(wrong -> right) drills for priors with **no existing wrong-form coverage**.

Ranked by frequency x family breadth (evidence = final-state failing generations in the mined batch):

1. **In-place array mutation across calls is lost (pass-by-value)** — ev. ~26 wrong-stdout
   (17 cs_quick_sort alone, identical unsorted output; plus bubble/merge variants); **10 families**
   incl. gpt-oss-120b, gemini-2.5-flash, trained a3b/q36. The single biggest compiles-but-wrong producer.
   - wrong: `quicksort(arr, 0, n-1); … println(arr[i]);` (Void sort mutating a param)
   - right: `fn quicksort(xs: Int[], …) -> Int[] { … ret xs; }` and `arr = quicksort(arr, …);`
2. **`toon_parse_file` called outside `fx`** — ev. 64 FX-001; **7 families** (command-r, gemma,
   granite, llama, mistral, qwen14b, qwen3). Existing fx drill only covers `println`.
   - wrong: `let doc: ToonDoc = toon_parse_file("data.json");` at statement level
   - right: `let doc: ToonDoc; fx { doc = toon_parse_file("data.json"); }` (guide §TOON example)
3. **Array slicing syntax** — ev. ~30 diagnostic-confirmed (54 sources contain it); **13 families**;
   dominant in cs_merge_sort (`arr[..mid]`, `right[j..]`) and BFS queue-pop (`queue = queue[1..];`).
   - wrong: `let left: Int[] = arr[0..mid];` / `queue = queue[1..];` / Python `left[i:]`
   - right: explicit loop-copy: `let left: Int[] = []; loop i in 0..mid { left = left + [arr[i]]; }`
4. **`else if` chain inside an if-expression** — ev. 28; **8 families**; classic fizzbuzz/classifier shape.
   - wrong: `let t: Text = if a { "x" } else if b { "y" } else { "z" };`
   - right: nested `else { if b { "y" } else { "z" } }`, or statement-level `if`/`else if` + assignment.
5. **Fixed-size array types and fill literals** — ev. 22 (`fixed-size array types are not supported`)
   + ~15 more inside array-literal/loop-range buckets; **8+ families** incl. trained q36
   (`let dp: Int[m+1][n+1];`). Forms seen: `Int[10]`, `[[Int; n]; n]`, `[0; 6]`, `[true; limit]`,
   `new Bool[30](true)`, Python comprehensions.
   - right: `let dp: Int[] = []; loop i in 0..n+1 { dp = dp + [0]; }` and the 2D row-append idiom
     (probe-verified `Int[][]` works). A dp2d *repair* pair would complement the positive 310_dp2d units.
6. **0-based string indexing** — ev. ~25 runtime `String index 0 out of bounds`; **9+ families**;
   plus cs_substring off-by-one wrong-stdouts (`pos` result printed as-is when task wants 0-based).
   - wrong: `s[0]`, `loop i in 0..length(s) { … s[i] … }`
   - right: `s[i+1]` / `loop i in 1..length(s)+1`; `pos(needle, s)` is 1-based, `0` = absent
     (subtract 1 when a task asks for a 0-based index); `copy(s, start, count)` 1-based.
7. **Array concatenation with `+`** — ev. ~8 runtime ARRAY+ARRAY; **4 families**; functional
   quicksort shape `ret sort(left) + [pivot] + sort(right);`. Extra-nasty: compiles, dies at runtime.
   - right: append loops; only `xs + [v]` (single-element literal append) is supported.
8. **Rust `match` statement** — ev. 17 sources; **8 families** (exaone, gemma, granite, llama,
   mistral, phi, qwen3, qwen3-coder); `match kind { "deposit" => {…} }` in ledger/FSM tasks.
   - right: `if kind == "deposit" { … } else { if … }` chain.
9. **Tuple-element arrays** — ev. 9 direct parse failures (eligibility_bool_logic across
   a3b/mistral/qwen3/qwen3-coder/granite); 44 still-fail sources use tuple types somewhere.
   - wrong: `let guests: (Int, Bool, Bool, Bool)[] = [(20, true, …), …];`
   - right: record type + append pattern (`ps = ps + [p];`, no nested record literals in one array literal).
10. **Format placeholders in `println`** — ev. 10 sources with `{}` (plus `%.2f` variants);
    4 families (exaone, gemma, granite, phi, also qwen3/llama via wrong-stdout: `10! = {}3628800`,
    `area=%.219.63…`, `7.500000E+01`).
    - right: variadic `println("10! = ", v)`; Real precision via `v:0:2` (extends the existing
      float-format pair with the placeholder-shaped wrong form, which it does not currently show).
11. **1-arg TOON getters on element nodes** — ev. 8 `SCOPE-001 toon_get_int`; **5 families**.
    - wrong: `let n: Int = toon_get_int(numberNode);` (element of a scalar array)
    - right: `toon_int_value(node)` for node values; `toon_get_int(node, key)` needs a key (probe-verified).
12. **Invented text builtins** — ev. ~12 SCOPE-001 (`string_at` 4, `text_len` 2, `substring`,
    `text_at`, `itos`, `array_slice`); 6+ families.
    - right: `s[i]` (1-based), `length(s)`, `copy(s, start, count)`, `int_to_text(n)`.

Recurring-but-covered (do NOT duplicate; consider a second variant only if the next trained board
still shows them): `return`->`ret` (26 sources, 8 fams; `repair_return_to_ret` exists),
`..=` ranges (29 sources, 6 fams; `repair_inclusive_range` exists), nested `fn` in `main`
(14, 6 fams; adjacent to `repair_contract_nested_in_main` but a plain-nested-fn variant is arguably new).

Note: drills 1, 3, 4, 5 were still being hit by the **trained** cs-aug2/q36 boards
(merge-sort slicing, eligibility tuple arrays, dp fixed-size declarations, fizzbuzz else-if-expr),
so they close known gaps in the current corpus, not just base-model noise.

## 4. LANGUAGE-GAP findings (flagging only — no fixes made)

Per the bucket-2 discipline (architecture doc §3.3): still-fails where the natural program arguably should work, or where failure mode is hostile.

- **`new Int[](5)` compiles, then VM-errors** `Array index must be an ordinal value`.
  Accepted-then-crash; should either allocate or be rejected at compile time.
  Repro: `fn main() -> Void { let xs: Int[] = new Int[](5); fx { println(length(xs)); } ret; }`
- **`xs + ys` array concat fails at RUNTIME, not compile time**, while `xs + [v]` is the blessed
  append. The asymmetry is unguessable and the late failure defeats the repair loop.
  Repro: `let c: Int[] = a + b;` -> runtime "Got ARRAY and ARRAY".
- **Array parameters are value copies while records are pointer-backed** (guide L408 vs probe p1).
  This asymmetry silently no-ops every in-place sort — 10 families produced identical
  compiles-runs-wrong output. Even a compile-time note ("array param mutated but never returned")
  would convert silent-wrong into a diagnostic.
- **No `else if` chain in if-expressions** (statement position supports it). 8 families wrote
  the natural chain form in expression position.
- **No array slicing** (13 families reached for it) and **no tuple-element arrays** (tuples exist
  as return types only). Both are candidates for the backlog, not the corpus alone.
- **Misleading diagnostic for nested `fn`**: a nested function definition reports
  `expected ')' to close argument list`, pointing nowhere near the real problem.
  Repro: `fn main() -> Void { fn inner(x: Int) -> Int { ret x + 1; } … }`

## 5. FIXED-SINCE (confirmation the 2026-07-01 batch closed historical classes)

- **Silent compile failures**: 190 historical rc=1 cases with empty stdout+stderr
  (06-27/06-28 binaries, 13 families) — 188 now produce proper `[SYN-001]` diagnostics.
  The diagnostics/AST-parser work landed; the old "no stderr" triage bucket is obsolete.
- **Record-method pointer arg bug**: mistral-small3.1 miner program failing
  `argument 1 to 'counter.increment' expects type POINTER but got VOID` now runs correctly.
- 3 old cs_quick_sort generations that runtime-errored on the 06-28 binary now run
  (still wrong output — they hit the pass-by-value prior, drill #1).
- The `n%2`/`n/2` Real->Int recursion coercion fix (pscal-core da08d77) predates this batch;
  no collatz-shaped arithmetic failures remain in the 06-28+ logs — nothing to re-teach.

## 6. HARNESS ARTIFACTS (excluded from corpus signal)

- **phi4 prose-as-source** (14 cases, all `unexpected token ','`): repair-round extraction
  captured explanation text ("To correct the issues in your previous attempt, …") instead of code.
  Fence-extraction bug on phi4's repair replies — harness fix, not corpus.
- **65 generation failures** (no source): qwen3.5-9b-mlx 28, ornith 6, qwen3:8b 6, trained q36 5, …
  — the known template/stop-token/eviction artifact classes.
- **Bench-marker leak** (2 exaone: `AETHER_BENCH_END` as identifier) — already covered by
  `repair_benchmark_marker_leak`.
- Wrong-stdout cases that are pure algorithmic misses (cs_nqueens wrong counts, hanoi move order,
  hard_sensor_streak run-length logic, dijkstra distances) are **model capability**, not corpus-fixable;
  they were excluded from the drill list. Teachable formatting subsets (trailing blank line via a
  final bare `println()`, one-value-per-line vs comma-joined) are folded into drills 1/10 shapes.
