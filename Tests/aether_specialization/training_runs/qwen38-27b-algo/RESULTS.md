# Qwen3.8-27B fine-tunes, no guide: three runs (2026-10-15/16)

Same base, recipe and seed; only the data changes. All graded on aether 2026-10-16-3 (the pilot's first
attempts re-graded with `tools/replay_bench.py`; its after-repair numbers are as scored on 2026-10-15-2).
One seed, one repeat: differences of one or two tasks are within noise.

| run | data added | stopped at |
|---|---|---|
| `qwen38-27b-l1-pilot` | L1 drills and positives (560 + 49) | epoch 0.87 |
| `qwen38-27b-toon` | + 61 TOON/MStream pairs, 7 drills (621 + 56) | epoch 0.84 |
| `qwen38-27b-algo` | + 70 algorithm-idiom pairs, 3 drills (691 + 59) | epoch 1.02 |

## First attempt (after repair)

| suite | pilot | toon | algo | guided Qwen3.6-27B (B0, medium guide) |
|---|---|---|---|---|
| tasks_v2_pos (35) | 31 (34) | 32 (34) | 32 (35) | 33 |
| tasks_hard_v2 (14) | 5 (10) | 14 (14) | 13 (14) | 13 |
| tasks_hard_nontoon (5) | 3 (5) | 3 (4) | 3 (4) | 5 |
| tasks_cs (19) | 16 (18) | 15 (18) | 16 (18) | 19 |
| **board total (73)** | **55 (67)** | **64 (70)** | **64 (71)** | **70** |
| sealed frontier (15) | 7 (9) | 7 (8) | 9 (11) | |
| sealed frontier_algo (14) | 5 (7) | 9 (11) | 10 (12) | |
| sealed frontier_spec (12) | 5 (8) | 7 (10) | 9 (11) | |
| sealed tasks_sealed_toon (12) | 1 (1) | 5 (8) | 4 (8) | |
| **sealed total (53)** | **18 (25)** | **28 (37)** | **32 (42)** | |

## Reading

- The algorithm pack fixed exactly the failures it targeted (sieve `step`, collatz if/else, lcs and
  edit-distance tables; heap sort now repairs), and the sealed suites rose again (28 to 32 first attempt,
  37 to 42 after repair). The board total did not move: three different cs tasks failed instead, each on a
  construct Aether did not have or reported badly (a `fn` nested inside main, `queue[1..]`, `ret f(x)` for a
  tuple-returning f).
- Those three became language changes the owner approved: open-ended slices and tuple forwarding, and a
  nested `fn` is now FUNC-001 with a fix-it hint (aether 2026-10-17-1). Hand-authored packs chase one habit
  per round; the next lever is distillation (`tools/aether_distill.py`): the guided base model solving
  hundreds of fresh tasks, kept where an independent Python solution agrees.
- Board scores were graded on 2026-10-16-3, before those language changes; tasks that failed only on them
  would pass today.
