# Running notes: OpenRouter twin of the B0 boards (fold into README.md, then delete)

- Purpose (owner, 2026-10-10): future boards run on OpenRouter, not local GPUs; this is their baseline. It is not
  the local panel (different weights/quantisation/serving), so later OpenRouter boards pair with THIS set.
- Same instrument as results/b0_20261007: frozen aether 6659679 binary (sha256 addfeb8d...), guides from that
  checkout (medium/small 2026-09-05-1, card 2026-10-07-1), tasks_frozen/ byte-identical to the local boards
  (sha256 5c9c6d73 / fa092145 / 388d2a8d / c38ff47a), live tasks_traps and re-timed tasks_scale, harness 20426ef7+,
  T 0.2, seed 42+r (set on the destinations: --seed-base only applies to self-hosted ones), 3 repeats, 2 repairs.
- Destinations: Tests/aether_doc_bench/destinations.openrouter_20261010.json, one pinned provider per model
  (allow_fallbacks false), chosen 2026-10-10 from /api/v1/models/<id>/endpoints:
    or-qwen36-35b-a3b       AkashML    fp8   output cap 96,000   cache read $0.05/M (in $0.10, out $0.90)
    or-qwen36-27b           Chutes     fp8   output cap 65,536   cache read $0.03/M (in $0.30, out $2.00)
    or-qwen35-9b            Parasail   bf16  output cap 96,000   no caching at bf16 (in $0.10, out $0.25)
    or-bonsai2-27b-ternary  Darkbloom  int4  output cap 32,768   cache read $0.037/M (in $0.075, out $0.50)
  devstral-small-2 and prism-coder-7b are not on OpenRouter. Output caps differ from the local 96,000 where the
  provider's own cap is lower.
- Boards (159 units): B0 48, B1 12, B0-small 24 (qwen35-9b + Bonsai), P0 24, S0 3 (qwen36-35b-a3b), BP 48.
- Runs on m4mini from ~/b0-pscal with RUN_DIR=Tests/aether_doc_bench/results/b0_openrouter_20261010,
  LANE_TAG=openrouter, no TRA_QUEUE_URL; key from ~/.config/pscal/openrouter.env (owner-provided, read-only).
- Caching: verify cached_tokens > 0 in the smoke run; record measured cache hit rate and cost per board.
- Smoke 2026-10-10 15:4xZ (smoke/or-qwen36-35b-a3b__smoke.json): 4/4 exact, 0 infra; seed 42 sent; reasoning on
  (915-2,410 reasoning tokens per answer); prompt caching confirmed: request 1 cached 0, requests 2-4 cached 15,840 of
  ~16,200 prompt tokens (98%); $0.0113 for the 4 cases. OpenRouter's generation record confirms provider AkashML,
  model snapshot qwen/qwen3.6-35b-a3b-20260415, cache_discount applied.
- Lane started 15:51Z on m4mini (screen b0or): MAXPAR=4 units x AETHER_BENCH_WORKERS=3 cases per unit in flight.
  First 2 minutes: 32 cases, 0 empty. Spend guard (screen b0orguard, ~/b0-tools/or_spend_guard.sh) pauses the lane
  at $32 of key usage (owner has $38 credit, $50/month key limit); usage log ~/b0-tools/or_spend.log on m4mini.
- 16:11Z: or-qwen36-35b-a3b B0 complete (12 units in 20 min): first-attempt 68/73 (r0), 72/73 (r1); spend $1.03 at
  16:06Z. The local 8-bit MLX build scored 68/73 on r0.

## 2026-10-11 12:05Z -- B0 complete for all four models; B1's first three units rejected on a provider 429

B0 finished at 12:05Z (48 units; key usage $8.63). The first three B1 units (or-qwen36-35b-a3b traps r0-r2)
failed the harness's model check with HTTP 429 from the pinned provider ("temporarily rate-limited
upstream") and were rejected with zero variants -- nothing scored. The lane carried on with the other models.
A second pass (`or_second_pass.sh` on the bench host, screen b0or2) starts when the first ends and re-runs
whatever has no report; it uses the current driver, which opens a model's remaining slots only after one
unit has passed its model check.

## 2026-10-12 11:26Z -- hosted Bonsai dropped (owner); B0-small in its own lane

- 07:46Z: B0-small started in a lane of its own (`or_small_lane.sh`), beside the main lane, because the plan's
  guide freeze waits on it and in the main lane it sat behind the slow traps board.
- 11:26Z: owner: "kill bonsai". Hosted Bonsai (one provider, int4, 4-7 h per hard-suite unit, well below the
  local ternary build) gets no further boards; its three B0 repeats stay as data. All three twin lanes were
  stopped with every shell frozen first, so no checkpoint was deleted, and restarted without it
  (`or_lane_nobonsai.sh main|small`, two passes each). In-flight requests were lost; units resume from their
  checkpoints. The parked qwen3.5-9b traps r1 unit was turned back into a checkpoint so only its unmeasured
  cases re-run. The hosted panel from here: qwen3.6-35b-a3b, qwen3.6-27b, qwen3.5-9b; B0-small rests on
  qwen3.5-9b alone.
- The qwen3.6-35b traps units the provider rate-limited on 2026-10-11 run in this pass.
- Key usage at the restart: $20.58 (guard pauses at $32).

## 2026-10-12 14:29Z -- a resumed unit was accepted short; audit of every unit

B0-small or-qwen35-9b hard_nontoon r2 reached [done] with 3 of 5 cases. It was resumed from a checkpoint
holding 3 finished cases; on the resume the harness's model check failed (a provider reply without choices),
the destination was skipped, and the harness wrote the report with just the kept cases. The driver's verdict
only asks for more than zero cases, so it accepted the unit. Found because its status line said fa=1/3.
The report was turned back into a checkpoint; the lane's second pass resumes it.

`audit_units.py` (one case per task of the suite, per guide variant) over every accepted unit: 165 units on
the bench host across the twin, the free survey and the local sets, and the 84 committed units -- this is the
only short one. Before the next board the driver's verdict must compare the case count with the suite, and a
resume whose model check fails must not write a report.

Also noted: qwen3.5-9b ends 24% of its small-guide first attempts (47 of 198) with no answer inside the
output budget (finish "length"), against 15% (33 of 219) with the medium guide on the same provider. Scored
as model failures under the no_answer rule; it accounts for most of the gap between its small- and
medium-guide first-attempt rates.

## 2026-10-12 ~15:00Z -- owner's no-retry rule; where it takes effect on this board set

Owner: "empty first attempt is a failure going forward. New rule ... Meaning, no retries." Harness 6a44b26d:
a first attempt with no program ends the case; no repair round is sent (`--repair-after-no-answer` gives the
old behaviour; a resume across the rule is refused).

On this board set the rule changes at a board boundary, never inside a board:
- B0, B1, B0-small finish on the harness they started on (20426ef7): empty first attempts were repaired.
- P0, S0 and the Python baseline run on the new harness.
`or_rule_switch.sh` on the bench host does it: stops the main lane when it starts its first P0 unit, waits for
the B0-small lane, runs the B1 leftovers on the old harness, takes only `tools/aether_doc_bench.py` from
origin/main (the clone holds the live results untracked, so it is not fast-forwarded), then runs P0, S0, BP.
The free survey shares that harness file and follows the rule for units started after the switch.

What the rule would have done to the boards already run ("FX nr" in the summaries): qwen3.5-9b B0 84.9% ->
77.6% after repair, B0-small 90.3% -> 71.3%, traps card 29.2% -> 16.7%; hosted Bonsai B0 93.2% -> 82.7%; the
two larger qwen models are unchanged within half a point.
