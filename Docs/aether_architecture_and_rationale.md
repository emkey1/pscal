# Aether Architecture & Rationale (as-built)

*Audience: human maintainers.* This document explains how Aether actually works
today and **why** specific implementation decisions were made — including the
awkward ones. It is deliberately separate from:

- [`src/aether/DESIGN.md`](../src/aether/DESIGN.md) — the forward-looking design
  *vision* (goals, phases, what Aether *should* become). Aspirational.
- [`Docs/aether_for_llms_and_others.md`](aether_for_llms_and_others.md) — the
  *reference guide* fed to models and humans who just want to write Aether.
  Descriptive, not rationale.

This document is the third leg: the *as-built* engineering rationale. If you are
about to change the compiler, add a builtin, edit the corpus, or wonder "why on
earth is it done *this* way," start here.

---

## 1. The one-sentence thesis

> Aether is optimized so that a language model can write **valid, correct Aether
> with no reference guide in its prompt**, and the benchmark suite — not taste —
> is the instrument that tells us when the language design is wrong.

Everything below follows from that sentence. "Compact and human-auditable" (the
DESIGN.md framing) is necessary but secondary. The hard, measurable target is
**no-guide generation correctness**, abbreviated everywhere as **`none`** (as in
the `none` documentation variant — no guide supplied).

---

## 2. The compile pipeline, as actually built

```
Aether source
  → Aether source-rewrite layer        (src/aether/translate.c)
  → Rea parser + semantic analysis     (src/rea/…)
  → shared PSCAL AST                    (src/ast/…)
  → shared bytecode compiler           (src/compiler/…)
  → shared PSCAL VM                     (src/vm/…)
```

Key facts a maintainer must internalize:

- **Aether is a front-end rewrite over Rea**, today. This is the bootstrap path
  DESIGN.md §6.2 calls "intentionally transitional." It is not yet a standalone
  lexer/parser. Practically: Aether syntax is translated to Rea-shaped source/AST,
  then everything downstream is the shared PSCAL pipeline. **There is no separate
  Aether VM, bytecode, or runtime.** That is a hard architectural rule.

- **One binary, two personalities.** The `aether` binary is built from
  `src/rea/main.c` with `PSCAL_FRONTEND_KIND == FRONTEND_KIND_AETHER`. At runtime
  `frontendGetKind()` distinguishes Aether from Rea so the *same* code can behave
  differently for each front end (see the writeln-spacing decision below, which
  keys off exactly this). Grep `FRONTEND_KIND_AETHER` to find every such fork.

- **Because the backend is shared, an Aether-motivated fix usually improves the
  whole suite** — but it can also break Rea/CLike/Pascal. Any change gated on
  `frontendGetKind() == FRONTEND_KIND_AETHER` is Aether-only and safe; any
  un-gated change in `src/ast`, `src/compiler`, `src/vm`, or `src/backend_ast`
  is cross-cutting. Know which kind you are writing.

The consequence of the Rea-rewrite bootstrap shows up most visibly in the
[known warts](#6-known-warts-be-honest): some Aether parse failures are really
Rea-grammar limitations leaking through the rewrite layer.

---

## 3. The benchmark is the design instrument

This is the most important idea in the project and the least obvious from the
code. Read this section before proposing any "the language should…" change.

### 3.1 What the benchmark is

`tools/aether_doc_bench.py` runs a fixed suite of **29 tasks**. Each task is a
natural-language spec; the model must emit an Aether program that compiles and
produces the expected stdout. Every task is run under several **documentation
variants**:

| Variant  | What's in the prompt                | What it measures                    |
|----------|-------------------------------------|-------------------------------------|
| `full`   | the complete reference guide        | ceiling: can it follow a spec?      |
| `small`  | a condensed guide                   | working memory under context budget |
| `none`   | **no guide at all**                 | **what the model internalized**     |
| python   | (baseline) write the task in Python | task difficulty floor               |

`none` is the KPI. A model scoring well on `none` has *learned the language*,
which is the entire point of the fine-tuning effort. When `none ≈ small ≈ full`,
the language has been internalized.

### 3.2 The inversion: a hard task is a language bug, not a model bug

The crucial mental flip:

> If a task can only be solved by a frontier-scale model, that is evidence the
> **language design** is wrong — not that the model is weak.

Aether's premise is that the language is easy for models to learn. So a task that
defeats every small/mid model is a defect *report* about Aether. Recent examples
of tasks that, at one point, **no** in-house tuned model could pass:
`release_board`, `toon_safe_nested_codes`, `config_validator`. Each became a clue,
not a scoreboard entry.

### 3.3 The repair loop

When a model fails a `none` task, triage the failing generation
(`tools/none_fail_detail.py <eval>.json none`) into one of two buckets:

1. **Wrong prior** — the model wrote something reasonable from another language
   (`new T{}`, `1..=3`, `parse_json(...)`, bracket-indexed tuples). The language
   is fine; the model guessed. **Fix:** add a verified `broken → fixed` repair
   drill to the corpus (see [§7](#7-corpus-core--per-family-overlays)).

2. **Language defect** — the natural, correct-looking program *should* work but
   doesn't (it crashes, needs verbose staging, or lacks an obvious builtin).
   **Fix:** change the language/runtime so the natural program works, then let
   every model benefit for free.

Bucket 2 is where the recent TOON and `clamp` work came from. Those are worked
through in [§5](#5-nitty-gritty-decisions-and-why). The discipline is: **prefer a
language fix over a corpus patch whenever the natural program is the correct one**
— a corpus patch teaches one family to tiptoe around a wart; a language fix
deletes the wart.

---

## 4. Source naming: say what the model expects, lower to what the VM has

A recurring pattern: Aether's *surface* name is chosen to match what a model
trained on mainstream languages will reach for, and the rewrite layer lowers it
to the PSCAL backend spelling.

- `print` / `println` (surface) → `write` / `writeln` (backend).
- `int_to_text(n)` (surface) → `IntToStr` (backend), and the front end infers its
  return type as `Text` (`src/aether/translate.c`, the `int_to_text` handling at
  the alias site and the return-type-inference site).
- `has_toon()`, `toon_*`, `task_spawn`, `ai_chat(...)` — compact source helpers
  in place of raw backend/extension builtin identifiers.

The rationale is always the same: a model emits the name it learned elsewhere; if
that name compiles and does the obvious thing, the `none` score goes up and the
corpus shrinks. Adding a well-chosen alias is often cheaper and more robust than
training the alias *out* of every model family.

---

## 5. Nitty-gritty decisions, and why

Each subsection states **the problem the benchmark surfaced**, **the decision**,
and **the rationale**. Commit hashes are given so you can read the actual diff.

### 5.1 `writeln` emits arguments verbatim (no magic spaces)

- **Problem.** Pascal/Rea `writeln(a, b)` inserts implementation-defined spacing
  between arguments. A model writing `println("x=", x)` expects `x=5`, but got
  `x= 5` (or worse), so output never matched and tasks failed for a reason that
  had nothing to do with the program's logic.
- **Decision.** `gSuppressWriteSpacing` (`src/rea/main.c:1084`):
  ```c
  gSuppressWriteSpacing = (frontendGetKind() == FRONTEND_KIND_AETHER) ? 1 : 0;
  ```
  Aether suppresses inter-argument spacing (verbatim concatenation, the Python
  `print(sep="")` mental model); Rea keeps its historical spacing.
- **Rationale.** Models trained on `print()`/template-string languages expect
  concatenation, not Pascal field formatting. Gated on `FRONTEND_KIND_AETHER`, so
  Rea is untouched. This is the canonical example of the "one binary, two
  personalities" fork.

### 5.2 `int_to_text()` exists at all

- **Problem.** Models reach for an int→string helper by an obvious name; `IntToStr`
  is a Pascal spelling they don't predict, so they invented `int_to_text`,
  `to_text`, `str(...)`, etc.
- **Decision.** Provide `int_to_text` as a first-class surface alias of `IntToStr`
  with `Text` return-type inference.
- **Rationale.** Cheaper to accept the obvious name than to retrain every family
  away from it. (Other guessed spellings are still corpus-repair territory; this
  one was common enough to bake in.)

### 5.3 TOON degrades on invalid handles instead of crashing  (`c30d16260`)

- **Problem.** TOON accessors took an integer handle into a yyjson document. A
  model writing natural defensive data code — read a node that might be missing,
  then ask for a field — would pass an "invalid" handle and the runtime
  **aborted**. The correct-looking program crashed, so models learned to write
  verbose multi-stage guard scaffolding, which then failed for *other* reasons.
- **Decision.** Remove the hard error on invalid handles in
  `src/ext_builtins/yyjson/yyjson_builtins.c`. Accessors now degrade: navigation
  returns "invalid," `toon_has_key` returns false, and the `_or` getters return
  their supplied default.
- **Rationale.** The natural, un-staged program is the *correct* program; punishing
  it with a crash is a language defect (bucket 2). After the fix, the natural
  solution to `toon_safe_nested_codes` compiles and runs, flipping it FAIL→PASS
  for the tuned 7B and turning other models' crashes into runnable output.

### 5.4 TOON dotted-path key access  (`da678a1b6`)

- **Problem.** Reading `server.name` out of nested data required descending one
  level at a time: `toon_key(root,"server")` → `toon_key(that,"name")` →
  `toon_get_text(...)`. Models naturally wrote `toon_get_text_or(root,
  "server.name", def)` and got the default back every time.
- **Decision.** When a key contains `.`, the accessors
  (`toon_get_*_or`, `toon_has_key`, `toon_key`) walk each dotted segment and fall
  back to the default / false on any missing segment. **Plain (non-dotted) keys
  are byte-for-byte unchanged** — the dotted path is an additive fast path. Both
  the existence check (`HasKey`) and the value lookup (`GetKey`) had to learn the
  navigation, or `_or` getters would disagree with themselves.
- **Rationale.** The dotted path is what models write and what humans read; the
  one-level-at-a-time descent was pure ceremony and a reliable `none` failure.

### 5.5 `clamp(x, lo, hi)` (and friends `min`/`max`)  (`efdc178a0`)

- **Problem.** Bounding a value made models write `if x < lo { x = lo }` chains —
  which then collided with the one-liner-`if` wart ([§6](#6-known-warts-be-honest))
  and failed to parse. The obvious builtin simply didn't exist.
- **Decision.** Add `clamp(x, lo, hi)`: all-integer args return `Int`, any real
  arg returns `Real` (mirroring `min`/`max`).
- **Rationale.** A one-call builtin sidesteps the whole error-prone branch-chain.
  See [§8](#8-anatomy-of-a-builtin-worked-example-clamp) — adding it correctly
  touches four sites, and getting that wrong is a classic foot-gun.

### 5.6 `fx { … }` effect blocks

- **Decision / rationale.** All observable effects (I/O, etc.) live inside `fx`
  blocks. This is the surface fence that makes `@pure` and the contract system
  meaningful: a pure function provably contains no `fx`. It is a *front-end
  semantic fence*, not a new VM frame — consistent with the shared-backend rule.
  The cost: `fx` shows up a lot in generated code, which is why its interaction
  with one-liner `if` is the most-felt wart.

### 5.7 One canonical spelling for ranges, structs, returns

The benchmark punishes synonyms: every alternative spelling is a coin-flip the
model can lose. So Aether deliberately admits **one** form and rejects the others:

- **Ranges are half-open only.** `loop i in 0..3` yields `0 1 2`. `0..=3` is
  **rejected** (SYN-001). Rationale: `..` vs `..=` is exactly the kind of
  off-by-one coin-flip that tanks `none`; pick the Rust/Python-`range` half-open
  form and make the other a hard error so the model is corrected, not silently
  wrong.
- **Struct fields are `name: Type;` (semicolon, one per line); init is
  `Type { field: val }`.** Verified form:
  ```aether
  type Point {
      x: Int;
      y: Int;
  }
  let p: Point = Point { x: 1, y: 2 };   // also: new Point() then field assign
  ```
  `new T{}` (the brace-after-`new` C#/Java prior) is **not** valid and is a
  documented corpus revert. Comma-separated fields silently parse as *no* fields
  (you get `FIELD-002 Unknown field` at the use site) — a sharp edge to be aware
  of.
- **`ret;` / `ret expr;`** is the one return form.

### 5.8 Contracts are executable, not decorative

`@pre`, `@post`, `@pure`, `@cost` lower to real checks/analysis (entry guards,
pre-return guards, purity analysis, frontend-validated budgets). A malformed or
detached annotation **fails in the front end** rather than degrading into a
comment. Rationale: a contract the compiler can't act on is a lie in the source;
the benchmark's contract tasks check that the guard actually fires. Tuple
post-conditions use dot indexing (`@post result.0 >= result.1`), **not** brackets
(`result[0]`) — another documented family revert.

---

## 6. Known warts (be honest)

A maintainer will hit these; pretending they don't exist wastes their day.

### 6.1 The one-liner `if` / inline-`fx` parse failure (SYN-001) — *fixed*

Historical; kept because it's the canonical illustration of the line-orientation
trap and how to fix that class of bug without touching the shared backend.

A guard written on a single line used to fail:

```aether
if x > 3 { fx { println("big"); } ret; }      // was: SYN-001 / SCOPE-001
```

while the identical multi-line guard worked. The cause was **not** the Rea
grammar (multi-line lowers fine) — it was the line-oriented rewrite layer's
one-construct-per-line assumption: `fx`/`ret` are only rewritten when the keyword
*leads* a line, so a one-liner left the mid-line `fx`/`ret` untranslated (`ret`
reached Rea verbatim → `SCOPE-001`; raw `fx {` → `SYN-001`).

**The fix** — `expandInlineBlockLine` in `translate.c`. When a line's leading
block construct (`if`/`else`/`while`/`for`/`loop`/`fx`) has its opening brace
matched **on the same line**, it is expanded into the canonical multi-line form.
Each header runs through `translateLineInMethod` (the main loop's header path)
and each **body statement** runs through the *same specialized per-statement
handlers the main rewrite loop applies, in the same priority order* — tuple
return (`translateTupleReturnLine`), return object-init / return-with-post,
array append (`translateArrayAppendLine`), then `translateLineInMethod` as the
fallback — followed by `applyJsonAliasesToLine` (so `toon_*` calls in conditions
still resolve). This is what makes a one-liner body lower **identically** to its
multi-line form: an earlier version translated body statements with bare
`translateLine`, which skipped those handlers, so a one-liner `ret (a, b);`
leaked `return (a, b);` (SYN-001) and `xs = xs + [v];` leaked an `ARRAY + ARRAY`
runtime error, even though the multi-line bodies lowered fine. The dispatch reads
`fnState`/`typeState` (the enclosing function/type context) but does not mutate
them, preserving the expander's self-contained property. The expanded chunk is
emitted via `trackRewriteOutputLines`, which maps every produced line back to the
**single source line** — so diagnostics keep their original Aether line numbers
(regression-tested). It is brace-balanced (net depth delta 0) and a strict no-op
for already-multi-line code: a differential over all 183 corpus candidates was
byte-identical before/after, and collapsing those candidates to one-liner form
(`Tools/aether_collapse_oneliners.py`) and recompiling reproduces each one's
multi-line output. It does **not** touch the shared Rea grammar.

This is why a pre-pass was the *wrong* shape (see §5-style reasoning): `lineNumber`
advances once per preprocessed line, so adding lines before the main loop would
desync the diagnostic line-map. Doing it *inside* the loop — one input line in,
many output lines out, all mapped to that input line — is line-map-correct by
construction, reusing the same machinery contract-annotation expansion already
relies on.

**Remaining one-liner gap:** a one-liner *type* declaration
(`type Point { x: Int; y: Int; }`) still doesn't register its fields
(`FIELD-002`). `type` is intentionally excluded from the expander (it's a
declaration, not a control-flow body); declare types multi-line. Repair drills
for the one-liner *guard* shape can now be dropped from the family overlays.

### 6.2 The bootstrap is the wart-factory

More generally: any surprising parse error that isn't obviously Aether-specific
is probably Rea grammar showing through. Reproduce it in a `.rea` file before
assuming it's an Aether bug.

---

## 7. Corpus: core + per-family overlays

The fine-tuning corpus is **not** one blob. See
[`Tests/aether_specialization/README_corpus_structure.md`](../Tests/aether_specialization/README_corpus_structure.md).

- **Core** (`corpus_candidates/`): model-agnostic verified positives. Everyone
  trains on these.
- **Per-family overlays** (`seed_repair_pairs.<family>.json`): remedial
  `broken → fixed` drills authored by probing *that family's actual `none`
  failures*. Different families fall back to different wrong priors, so a corpus
  tuned to one under-serves another. Empirically (`none`/29, fixed compiler):
  Qwen2.5-Coder 24 > Qwen3-4B 23 > Granite-8B 20 — the score falls with distance
  from the tuned family.

**Rule:** every `fixed_source` in an overlay must compile to its
`expected_stdout`. There is a verification step (run the fixed sources, diff
stdout) and it is non-negotiable — a broken "correct" example teaches the wrong
thing. When you add a language feature that obsoletes a wrong prior (e.g. dotted
paths), prefer deleting the prior at the language level over adding a drill that
teaches models to avoid it.

---

## 8. Anatomy of a builtin (worked example: `clamp`)

Adding a builtin **looks** like one edit and is actually **four**, plus a trap.
This tripped up the `clamp` work and will trip up the next person.

A builtin is recognized and executed through *separate* mechanisms:

| Site | File | Purpose | Symptom if you forget it |
|------|------|---------|--------------------------|
| 1. Definition | `src/backend_ast/builtin.c` (`vmBuiltinClamp`) | the C implementation | link/declare errors |
| 2. Header decl | `src/backend_ast/builtin.h` | visibility | compile error |
| 3. VM dispatch table | `vmBuiltinDispatchTable[]` in `builtin.c` | **runtime** name→handler | builtin "not found" at execution |
| 4. Builtin registry | `populateBuiltinRegistry()` in `builtin.c` (next to `"Max"`/`"Min"`) | **compile-time** "is this a builtin?" | **`Runtime Error: Undefined global variable 'clamp'`** |
| 5. Return-type inference | `getBuiltinReturnType()` in `src/ast/ast.c` | typed-context inference | `TYPE_VOID`/`UNKNOWN_VAR_TYPE` in typed bindings |

**The trap.** The dispatch table (site 3) drives *execution*; the registry (site
4) drives *compile-time name resolution*. They are not the same list. A builtin
present only in the dispatch table still resolves as an **undefined global** at
the front end — the symptom is a *runtime* "Undefined global variable" because the
compiler emitted a global load instead of a builtin call. `clamp` was in the
dispatch table and the return-type list and *still* failed until it was also
registered in `populateBuiltinRegistry` (where `max`/`min` live as `"Max"`/`"Min"`
— note the capitalization; lookups canonicalize to lowercase, so surface `clamp`
matches a `"clamp"` or `"Clamp"` registration equally).

Checklist for the next builtin: **define → declare → dispatch → register →
infer-type → rebuild → test a typed binding** (`let n: Int = clamp(105,0,100);`,
not just a bare call, so you exercise the return-type path).

---

## 9. Map of the territory

| You want to… | Go to |
|---|---|
| Understand the *vision* / phases | `src/aether/DESIGN.md` |
| *Write* Aether (reference) | `Docs/aether_for_llms_and_others.md` (and the `…small_contexts` variant) |
| Run the KPI benchmark | `tools/aether_doc_bench.py` (variants `full`/`small`/`none`, `--python-baseline`) |
| Triage a `none` failure | `tools/none_fail_detail.py <eval>.json none` |
| See the surface↔backend rewrite | `src/aether/translate.c` |
| Find Aether-only behavior forks | grep `FRONTEND_KIND_AETHER` |
| Add/modify a builtin | `src/backend_ast/builtin.{c,h}`, `src/ast/ast.c` — see [§8](#8-anatomy-of-a-builtin-worked-example-clamp) |
| TOON / yyjson bindings | `src/ext_builtins/yyjson/yyjson_builtins.c` |
| Corpus structure & overlays | `Tests/aether_specialization/README_corpus_structure.md` |
| Run the language regression suite | `Tests/run_aether_tests.sh` (or `ctest -R aether_tests`) |

---

## 10. The through-line

Aether is a thin, opinionated front end over the PSCAL backend whose every design
choice is answerable to one question: **did the no-guide benchmark go up?** When a
small model fails a task, the first hypothesis is that *Aether* is wrong — too many
spellings, a missing obvious builtin, a crash where degradation belonged, ceremony
where a dotted path belonged. Fix the language and every model gets better for
free; patch the corpus only when the model's prior, not the language, is the
thing at fault. The warts are debts owed to the Rea-rewrite bootstrap — but, as
the one-liner-`if` fix (§6.1) showed, most are payable *without* a new parser, by
normalizing the surface into the form the existing line machinery already
handles. Forking the shared grammar is the last resort, not the first.
