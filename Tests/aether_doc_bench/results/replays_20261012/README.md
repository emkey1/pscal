# Paired replays of L0 and L1 on the B0 baseline, the fx gate number, KPI levels (2026-10-12)

Zero-GPU follow-ups that the committed baseline unblocks (plan M2 exit and M3:
B_L0, B_L1, the D12 number, the inputs W8-27 needs). Every stored attempt of
the baseline boards was recompiled and re-run with each release's compiler by
`tools/replay_bench.py`, old and new graded against the same task definition.

Inputs: the unit reports of `../b0_20261007/` (B0, B0-full, B1 including the
unit in infra_pending, P0), `../b0_devstral_mlx4bit_20261011/` (B0, B1, P0) and
the hosted twin's finished B0 and B1 units (`../b0_openrouter_20261010/`, not
yet committed; that board set is still running). 124 reports, 2,523 first
attempts, 1,486 repair attempts, 2,723 distinct programs. Baseline compiler
2026-10-06-1 (aether 6659679).

| compiler | built from | binary sha256 |
|---|---|---|
| L0, 2026-10-07-1 | aether 7f58a33, Release, its own pins (pscal-core 2cb751d, rea b1320cf) | `8317d26d4866a8dc…` |
| L1, 2026-10-08-1 | aether 510b048, Release | `f986cbb4ba05eddf…` |

## B_L0 and B_L1

| | first attempts: pass before → after | fail→pass | pass→fail | into silent-wrong | repair attempts: pass before → after | pass→fail (repairs) |
|---|---|---|---|---|---|---|
| **B_L0** | 1,676 → 1,703 | 27 | 0 | **0** | 330 → 362 | 0 |
| **B_L1** | 1,676 → 1,705 | 32 | 3 | **0** | 330 → 352 | 12 |

- **S gate (no unwaived move into silent-wrong): met by both releases.**
- **R gate (no unwaived pass→fail): L0 meets it outright. L1 has 15 pass→fail
  attempts, 8 distinct programs, all stopped by a break the L1 changelog
  declares:** NAME-001 for a parameter or local that reuses a top-level const's
  name in another letter case, or a loop variable that reuses an outer
  variable's name (D17; 7 programs), and DIV-002 for `x // 5` (W8-14; 1
  program). `B_L1_waivers.json` is the tracked waiver file plus a proposed
  2026-10-08-1 block naming those 8 programs with the diagnostic each now gets;
  it validates (`replay_bench.py --check-waivers --waivers …`). With it adopted,
  R is 0. It is a proposal: the tracked `replay_waivers.json` is unchanged.
- **What L0 fixed, on real model programs:** all 27 first-attempt gains are trap
  tasks: `trap_literal_product` 11 (silent-wrong → pass), `trap_exit_status`
  10, `trap_life_continue` 6. The plan's exit criterion names the nested-continue
  and INT32 traps; both are among them.
- **What L1 adds:** the same 27 plus `tuple_recursive_digits` 2,
  `toon_parse_file_simple` 1, `hard2_sensor_windows` 1, `trap_grid_clone` 1; and
  20 first attempts move from silent-wrong to a coded error (the traps L1 turns
  into diagnostics). 14 uncoded errors become coded.
- Neither release moves first-attempt pass on tasks_v2_pos + tasks_cs by more
  than one case for any model (table in `KPI_baseline.md`): their effect is on
  the traps and hard tasks, as intended.

Proposed Replay lines for the changelog:

- 2026-10-07-1 (L0): `Replay (B_L0, 2,523 first attempts of the B0/B1/P0 baseline): pass 1,676 → 1,703, fail→pass 27, pass→fail 0, into silent-wrong 0.`
- 2026-10-08-1 (L1): `Replay (B_L1, same attempts): pass 1,676 → 1,705, fail→pass 32, pass→fail 3 (15 over all attempts; 8 programs, all NAME-001 / DIV-002 declared above, waived), into silent-wrong 0; silent-wrong → coded 20.`

Files: `B_L0.json`, `B_L1.json` (tallies, every pass-boundary flip, the gate),
`B_L0.log`, `B_L1.log`.

## D12: the fx gate number

The plan keeps FX-001 an error unless it is at least 5% of first-attempt
failures, in which case the A/B runs. On the medium-guide B0 units
(`fx_gate.py`, `fx_gate.txt`):

| | first attempts | failures | first diagnostic FX-001 | share of failures |
|---|---|---|---|---|
| all models | 1,898 | 534 | 90 | **16.9%** |
| without prism-coder-7b (which mostly returns prose) | 1,679 | 328 | 85 | **25.9%** |

The threshold is crossed several times over, so the A/B is due. Two facts shape
it: 89 of the 90 are programs that read a file (`toon_parse_file` outside
`fx`), and they are concentrated in two models (devstral 35–40% of its
failures, qwen3.5-9b 28%); the two larger qwen models and Bonsai never make
the error on the hosted boards. That points at the guide's file-reading
example as much as at the rule.

## KPI levels on the baseline (`KPI_baseline.md`, `kpi_baseline.py`)

Against the plan's provisional targets:

| KPI | provisional | baseline |
|---|---|---|
| G1, first attempt, v2_pos + cs | ≥ 90% | hosted panel pooled 90.3% (585/648): qwen3.6-27b 98.8, qwen3.6-35b 96.9, Bonsai 92.6, qwen3.5-9b 72.8. Local: Bonsai 93.8, devstral 75.3–75.6 |
| Gf, after repair | ≥ 97% | hosted panel pooled 97.5% (632/648); qwen3.5-9b 91.4, devstral 84–88 |
| S level, silent-wrong first attempts | ≤ 2% | hosted panel pooled 1.7% (15/876); devstral 12.8–17.9%, local Bonsai 3.7% |
| S, moves into silent-wrong on replay | 0 | 0 for L0 and L1 |
| R, unwaived pass→fail | 0 | L0: 0. L1: 15, all declared breaks (0 with the proposed waivers) |
| TRAP-SW suite first attempt | ≥ 80% | medium 43.8% (84/192), card 26.6% (51/192), 8 units |
| P1, card gap to G1 | ≤ 10 points | devstral 26.5; prism-coder 6.2 at a floor; hosted card board not run yet |

Observations for W8-27 (the targets are the owner's to fix):

- The provisional G1 and Gf thresholds sit almost exactly on the hosted
  panel's baseline (90.3%, 97.5%). As written they mean "do not regress", not
  "improve". The spread inside the panel is the real story: three models are
  at 93–99%, one at 73%.
- Pooled panel numbers hide that. A per-model floor (for example no panel model
  below its own baseline minus its repeat spread) would bind where a pooled
  target does not.
- TRAP-SW at 80% is far from the baseline. On replay L0 lifts the medium-guide
  traps from 84 to 100 of 192 first attempts (52%) and L1 to 101; the card from
  51 to 62 and 61 (32%). The rest has to come from the guides and L2.
- P1 cannot be set from one model. The hosted card board is queued.
- prism-coder-7b should not be in any panel KPI: it mostly does not return a
  program, so it measures nothing about the guide or the compiler.

## Still owed

- Per-trap silent-wrong after L1 (the replay report keeps pass-boundary flips
  per case but only tallies for class-to-class moves).
- The fx A/B itself, as a replay on the experiment arm.
- Re-run of this directory when the hosted twin's remaining boards (traps for
  the other models, card, small guide) are committed.
