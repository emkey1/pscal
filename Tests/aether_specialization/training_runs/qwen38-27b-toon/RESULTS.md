# Qwen3.8-27B fine-tunes, no guide: pilot vs TOON/MStream run (2026-10-15/16)

Two LoRA runs of the same recipe on the same base, one variable changed: the data.

| | `qwen38-27b-l1-pilot` | `qwen38-27b-toon` |
|---|---|---|
| dataset | `l1-20261015`: 560 instruction + 49 repair | `toon-20261016`: 621 + 56 (+61 TOON/MStream pairs, +7 drills from the pilot's own TOON/MStream errors) |
| recipe | 4-bit LoRA r=32 a=64, lr 1e-4, seed 3407, early stopping (patience 2, 12-record eval) | same |
| stopped at | epoch 0.87 (best eval loss 0.117, checkpoint 55) | epoch 0.84 (best 0.146, checkpoint 60) |
| training time | 27 min on one GB10 | 28 min |

Scoring: no guide, T=0, seed 42, one repeat, two repair rounds, generous output budget, thinking off.
FA = first attempt exact, FX = exact after repairs, counts of tasks.

## Board suites (73 tasks; the pilot's failures here were mined for the second run's drills)

| suite | pilot FA / FX | toon FA / FX |
|---|---|---|
| tasks_v2_pos (35) | 31 / 34 | 32 / 34 |
| tasks_hard_v2 (14) | 5 / 10 | **14 / 14** |
| tasks_hard_nontoon (5) | 3 / 5 | 3 / 4 |
| tasks_cs (19) | 16 / 18 | 15 / 18 |
| **total (73)** | **55 (75%) / 67 (92%)** | **64 (88%) / 70 (96%)** |

## Sealed suites (53 tasks; never mined, reported in aggregate)

| suite | pilot FA / FX | toon FA / FX |
|---|---|---|
| tasks_frontier (15) | 7 / 9 | 7 / 8 |
| tasks_frontier_algo (14) | 5 / 7 | 9 / 11 |
| tasks_frontier_spec (12) | 5 / 8 | 7 / 10 |
| tasks_sealed_toon (12, TOON + MStream) | 1 / 1 | **5 / 8** |
| **total (53)** | **18 (34%) / 25 (47%)** | **28 (53%) / 37 (70%)** |

For scale: the untrained base scores FA 0 / FX 2 of the 73 board tasks without a guide; with the medium
guide, untrained Qwen3.6-27B scored 95.9% FA and Bonsai 2 (Qwen3.8 at 1.7 bits) 91.8% (B0, 2026-10-07).

## Graders and hosts

- `qwen38-27b-toon`: graded on aether 2026-10-16-3 (96f099f). Board suites served by vLLM (bf16) on claw3;
  sealed suites by Ollama (bf16 import of the same merged export) on mbp5, files `m5ollama__*`. The three smoke
  tasks gave byte-identical first attempts on both hosts at T=0.
- `qwen38-27b-l1-pilot`: graded on 2026-10-15-2 on claw3 (frontier with `--allow-broken-oracle`: Linux `val("")`
  then reported success, so `val_checked_ledger` was unachievable there). Its first attempts were re-graded on
  2026-10-16-3 with `tools/replay_bench.py`: every suite unchanged. After-repair figures are as scored, since
  repairs depended on that grader's feedback.
- The pilot's untrained base: `base__*` (2026-10-15-2).

## Reading

- The targeted gap closed on the board: hard_v2 first attempts 5 to 14 of 14, the misses having been TOON API
  recall (invented names, getter vs value accessor, parse_file outside fx).
- It also moved on the sealed TOON suite (1 to 5 of 12 first attempts, 1 to 8 after repair), so this is not only
  the board: the API knowledge generalised in part. The gap between 14/14 on hard_v2 and 5/12 sealed says the
  sealed tasks are harder or that the drills fit the board's TOON patterns; more varied TOON data is the next lever.
- frontier_algo rose 5 to 9 with no algorithm data added; with one seed and one repeat, differences of one or two
  tasks are within noise. Three seeds per configuration before trusting a small difference.
- Still short of the guided base on the board (88% vs 92-96% FA).
