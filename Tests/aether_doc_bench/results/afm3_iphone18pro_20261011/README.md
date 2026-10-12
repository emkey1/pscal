# Apple's on-device model on an iPhone 18 Pro (A20), and guide drafts for it, 2026-10-11

Follow-up to `../afm3_20261008/` (the iPad run; read its README for the bridge
and the method). Same frozen compiler as the B0 boards (aether 6659679, language
2026-10-06-1, binary sha256 `addfeb8d…acc9b66`), T 0.2, seeds 42 + repeat, 3
repeats, 2 repair rounds. The phone serves the variant **AFM 3 Core Advanced**
with an **8,192-token** window (the iPad: AFM 3 Core, 4,096), logged every five
minutes in `variant_log.txt`. `RUN_LOG.md` is the record as written, including
three interruptions (phone unplugged, a Python upgrade under a running board, a
switch from the USB tunnel to the wireless address); none left an unmeasured
case in the tables. Reports are gzipped with the device address replaced.

Owner's direction part-way through: only the simple suite (tasks_v2_pos, 35
tasks) matters until it is near perfect. Everything after the card board is that
suite only.

## Results on the simple suite, 105 cases per row

| guide | prompt tokens (median, measured) | first attempt | 95% (Wilson) | after repair | silent-wrong | compile errors |
|---|---|---|---|---|---|---|
| card-v1 | 2,017 | 10 | 5–17% | 11 | 22 | 73 |
| v6 (the iPad run's best) | 2,301 | 4 | 1–9% | 7 | 23 | 78 |
| **g3500** | 3,451 | **28** | 19–36% | 31 | 19 | 58 |
| g3700 | 3,713 | 21 | 13–29% | 33 | 30 | 54 |
| g4000 | 3,948 | 19 | 12–27% | 26 | 36 | 50 |
| Python, no guide | 212 | 41 | 30–49% | 47 | 25 | 39 |

`guide_boards/comparison.md` has the per-task grid and the first errors per
guide (`guide_boards/compare.py` regenerates it).

Split by whether Python can express the task (11 of the 35 import an Aether
module file, use the JSON or memory-stream calls, or ask for contracts):

| | 24 language-neutral tasks (72 cases), FA / FX | 11 Aether-specific tasks (33 cases), FA / FX |
|---|---|---|
| Python, no guide | 38 / 44 | 3 / 3 |
| card-v1 | 10 / 11 | 0 / 0 |
| v6 | 4 / 7 | 0 / 0 |
| g3500 | 16 / 19 | 12 / 12 |
| g3700 | 12 / — | 9 / — |
| g4000 | 13 / 18 | 6 / 8 |

The card board's hard suite (`board_card/`, tasks_hard_v2, 42 cases): 0 first
attempt, 0 after repair. Its other suites were not run on the phone.

## What it shows

- **The model is the main limit.** With no guide and in Python it gets 53% of
  the language-neutral simple tasks first time. That is the practical ceiling;
  "near perfect" is not available from this model in any language.
- **A longer guide that leads with a whole program and covers the features the
  tasks use roughly triples the card** (28 against 10 first attempt; the
  intervals do not overlap), and is what makes the Aether-only tasks reachable
  at all (12 of 33 against 0).
- **The iPad result did not transfer.** v6, which took the smaller iPad model
  from 9 to 21 on held-out simple tasks, scores below the card here.
- **The three long drafts cannot be ranked.** g3500 against g4000 is 17 wins to
  8 (sign test p = 0.11), against g3700 14 to 7 (p = 0.19). The three repeats of
  a task are nearly the same program at T 0.2, so the effective sample is about
  35. Whole tasks flip between drafts; 15 tasks pass first time under at least
  one draft, no draft holds all 15, and 20 never pass first time.
- **What moved, what did not.** `return` as the first error fell from 23 cases
  (card) to 5–9; `import` errors disappeared. Output shape did not move: values
  asked for on separate lines printed on one, a missing space, `1..n` for an
  inclusive range, the expected output appended after the program. Rewriting
  every example to print one value per line (g3700) did not fix it.
- Phone against iPad on the card, same prompts and seeds: 0 of 66 identical
  programs; the phone is deterministic for a given prompt and seed. Different
  weights, similar level (10 against 9 first attempt on the simple suite).

## The drafts (`guides/`)

Proposals only; nothing in the aether repository was edited.

| file | chars | what |
|---|---|---|
| `v6_table_plus.md` | 6,544 | the iPad run's v6, byte-identical |
| `g3500.md` | 10,449 | a complete program and five rules first; v6's "write / never write" table with rows for `use`, `!`, inline `if`; worked programs for contracts, real formatting, modules, JSON reading, memory streams, extension methods, array scans |
| `g3700.md` | 11,341 | g3500 with every example printing one value per line, three added rules, and g4000's closing checklist |
| `g4000.md` | 12,138 | g3500 plus a loop-with-break program, a second JSON program, and a five-line checklist at the end |

Every code block in the three drafts runs on the frozen binary and prints
exactly the text block that follows it (`guides/verify_guides.py`), and
`tools/audit_guides.py --strict` finds no benchmark identifier in any of them.

**Caveat.** The drafts were written after reading this suite's tasks and the
phone's failures on them, so tasks_v2_pos is a development set for them. The
gain over the card is large enough to believe in direction; its size needs
confirming on tasks the drafts were not written against.

## For the guides

- A "whole program first, then rules" opening and explicit coverage of modules,
  JSON reading and contracts are worth testing in a card-v2 on the panel; the
  card has none of the three and a third of the simple tasks need them.
- Do not expect a translation table to stop `return` on a small model, here or
  on the iPad; a first-position rule and examples did more.
- Past about 3,500 tokens there was no further gain on this model.
