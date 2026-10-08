# Apple on-device model (AFM 3 Core) and the 4K-token guide tier, 2026-10-08

Apple's on-device Foundation Model, served from an M4 iPad Pro (iPadOS 27.0,
24A437) by `tools/apple_fm_bridge`. The system reports the variant `AFM 3 Core`
and a 4,096-token context shared by instructions, prompt and answer. Every run
here uses the frozen pre-L0 compiler (aether 6659679, language 2026-10-06-1), T 0.2,
seeds 42+r, 2 repair rounds. The model is a small general-purpose model, not a
coding model; the point is what a guide can do for a model this size, and where
the 4K window stops it.

Reports are gzipped harness JSON with local paths and the device address
replaced (`<scratch>`, `<ipad-bridge>`); `gzip -dc` one to read it.

## 1. The card board (`board_card/`)

card-v1 (`docs/aether_card.md`, 1,789 o200k tokens) over the B0 suites and the
traps, 3 repeats, 291 cases, 0 infrastructure failures:

| suite | first attempt | after repair | silent-wrong |
|---|---|---|---|
| tasks_v2_pos | 9 / 105 | 12 | 24 |
| tasks_hard_v2 | 0 / 42 | 0 | 0 |
| tasks_hard_nontoon | 0 / 15 | 0 | 0 |
| tasks_cs | 0 / 57 | 0 | 3 |
| tasks_traps | 0 / 72 | 0 | 2 |
| **all** | **9 / 291 (3.1%)** | **12** | **29** |

Almost every failure is coded. First errors: `return` 41, `const` holding file
data 25 (the card never says how to read an input file, so the model pastes
invented object literals), other SYN-001 shapes (`for`, `+=`, `Float`, `String`,
`fn f(): Int`), SCOPE-001 for invented builtins. Hard tasks get one attempt in
practice: their repair prompts (guide + task + failed program + error) do not fit
the window, and the harness's context guard refuses them.

## 2. Guide variants (`guides/`, `guide_experiment/`)

Development set: 16 tasks (10 tasks_v2_pos, 3 file-reading tasks_hard_v2, 3
tasks_cs) × 2 repeats. Every code block in every variant compiles and prints its
stated output on the frozen binary (`tools/verify_guide_snippets.py --run-outputs`).

| variant | o200k | first attempt | after repair | what it tests |
|---|---|---|---|---|
| v0_card | 1,789 | 0 / 32 | 3 | baseline |
| v1_card_table | 2,087 | 4 / 32 | 6 | a "write / never write" table at the top (`ret` not `return`, `loop` not `for`, `x = x + 1` not `+=`, `Real`/`Text` not `Float`/`String`, no `.push`/`len`, no data in `const`, no placeholders) |
| v5_card_toon | 2,026 | 0 / 32 | 0 | a "reading a JSON file" section |
| v2_card_table_toon | 2,324 | 5 / 26 sent | 5 | both; 6 first attempts did not fit the window |
| v3_template | 1,039 | 0 / 32 | 0 | one complete program instead of many snippets |
| v4_minimal | 441 | 0 / 32 | 0 | the shortest guide |
| **v6_table_plus** | 2,157 | **4 / 30 sent** | **11** | v1 plus rows for `fn f(): Int`, `type X = ...`, comma-terminated fields, inline object types |
| v7_table_last | 2,092 | 0 / 32 | 5 | v1's table moved to the end of the guide |
| v8_table_lean | 1,542 | 2 / 32 | 7 | v1 without the tuples section and the diagnostics table |

What the variants show:

- **The translation table is the change that works**, and only at the top: the
  same table at the end (v7) scored 0. Small models weight what comes first. It
  does not stop `return` (section 4); its other rows carry the gain.
- **The worked examples matter.** The model copies the shape of what it sees:
  v3's example opens with a `type`, and its programs opened with
  `type Int = Int;` and `fn f(n: Int): Int`; v4 shows too little and `return`,
  comma fields and invented `{name: Text}[]` types come back.
- **Room is the binding constraint.** The TOON section helps nothing on its own
  and, with the table, pushes long tasks out of the window. Trimming (v8) frees
  room for repair but loses more than it gains.

## 3. Held-out confirmation (`heldout_v6/`)

v6 on the 57 tasks the experiment never used, 3 repeats, matched case by case
(task and seed) to the card board:

| | v0 card | v6 table |
|---|---|---|
| first-attempt pass | 9 | **21** |
| silent-wrong | 15 | 12 |
| coded / uncoded error | 138 / 9 | 132 / 3 |
| passing after repair | 10 | 21 |

14 cases pass only under v6 and 2 only under v0 (two-sided sign test p = 0.004).
Every gain is on tasks_v2_pos (9 → 21 of 75, silent-wrong 12 → 3); the hard,
nontoon and cs suites stay at 0 under both. On tasks_cs silent-wrong rises 3 → 9,
mostly programs that print nothing and exit 0 (a top-level script ending in
`ret (arr);`); recompiling v6's generations on the L1 compiler (2026-10-08-1)
catches one of them, so that shape is W4-03's.

## 4. `return` as an alias of `ret` (D7)

An off-by-default experiment arm (`AETHER_EXPERIMENT=return=alias`) recompiled
stored first attempts with the same compiler, alias off and on:

| | B0 panel so far (249 first attempts, 4 models) | AFM 3 Core board (291) |
|---|---|---|
| pass | 214 → 214 | 10 → 12 |
| silent-wrong | 13 → 13 | **30 → 39** |
| coded | 21 → 21 | 244 → 232 |

No panel model writes `return`, and for the one model that does, the alias turns
more coded failures into silent wrong answers (9) than into passes (2): past the
keyword its programs carry semantic bugs that the coded error used to stop. D7
keeps `return` in class 2.

The guide does not cure the habit either: on the held-out set `return` is the
first error in 26 cases under both v0 and v6. The table's gain comes from its
other rows (loops, compound assignment, type names, return-type syntax, data in
`const`).

## Takeaways for the guides

- For a 4K-window model, a guide is: the translation table first, then compact
  worked examples, inside about 2,200 o200k tokens so a typical task and a repair
  round still fit.
- A `card-v2` with v6's table is a candidate for the next guide pass, measured
  beside card-v1 on the panel (P0's card arm shows whether panel models gain;
  they rarely write the shapes the table targets).
- Hard and file-reading tasks are out of reach at 4K: the prompt alone nearly
  fills the window and repairs do not fit. Re-run when a device offers a larger
  window or the `coreAdvanced3` variant; the bridge reports both in `/v1/models`.
