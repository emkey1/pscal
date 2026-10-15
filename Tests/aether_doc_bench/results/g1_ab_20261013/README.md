# Guide A/B: current medium guide against the first guide-pass draft (2026-10-13)

One board, both guides in each report and interleaved per task, so every task is
answered under both guides by the same model with the same seed.

| | |
|---|---|
| compiler | aether d8f0af5, language 2026-10-13-1 (L1 plus the slice-of-field fix), Release, sha256 `4d7b6cf6…030f8e` |
| current guide | `aether_for_llms_medium_contexts.md` 2026-09-05-1, sha256 `aebfc0f5…596a20` |
| draft guide (G1 full) | aether branch `draft/g1-full` at `d8f81ef` (not merged), medium stamp 2026-10-13-3, sha256 `ddca1a5e…6e7489`, about 1,100 tokens shorter |
| models | qwen3.6-35b-a3b and qwen3.6-27b on OpenRouter, pinned providers as in `../b0_openrouter_20261010/` |
| tasks | tasks_v2_pos, tasks_cs, tasks_hard_v2, tasks_hard_nontoon (73) and tasks_traps (24), current manifests |
| settings | T 0.2, seed 42, one repeat, 2 repair rounds, the no-retry rule for empty first attempts |

Every unit holds every case for both guides with none unmeasured (two units needed a
second pass after rate limits). Cost $4.09.

## Results, first attempt (after repair in brackets)

| suite | qwen3.6-35b current | qwen3.6-35b draft | qwen3.6-27b current | qwen3.6-27b draft |
|---|---|---|---|---|
| tasks_v2_pos (35) | 34 (35) | 34 (35) | 35 (35) | 35 (35) |
| tasks_cs (19) | 18 (19) | 18 (19) | 16 (19) | **19** (19) |
| tasks_hard_v2 (14) | 13 (14) | 13 (13) | 14 (14) | 14 (14) |
| tasks_hard_nontoon (5) | 5 (5) | 5 (5) | 5 (5) | 5 (5) |
| **73-task board** | 70 | 70 | 70 | **73** |
| tasks_traps (24) | 15 (19) | **18** (23) | 16 (21) | **19** (23) |
| silent-wrong first attempts, all 97 | 9 | 3 | 5 | 2 |

Case by case (task and seed matched), the first attempt differs in 15 cases: 12 pass
only under the draft, 3 only under the current guide (two-sided sign test p = 0.035).
The draft's gains repeat across both models on the division traps
(`trap_div_round_down`, `trap_factorial_literal`, `trap_digit_places`), where the
current guide's "`Int / Int` is integer division" is false on this compiler and the
draft says to use `div`; qwen3.6-27b also gains three cs tasks (`cs_factorial`,
`cs_nqueens`, `cs_dijkstra`). The draft lost `cs_edit_distance` and `two_type_methods`
on qwen3.6-35b and one hard_v2 case after repair.

## Limits

- One repeat on two strong models. The direction is consistent across both models;
  the size is not established.
- The draft was written after reading the baseline's failures (the division and
  naming fixes are accuracy corrections against the compiler; the `fx` file-read and
  range additions target failures seen on the baseline), so the 73-task board is
  partly in-sample for it. The traps suite was not used to write it.
- Only the full draft was measured, not the accuracy-only variant; the plan's
  adoption rule compares both.
