# Local-model tier board — 2026-08-11

First run of the fresh era. aether **`2026-08-09-1`** (`/usr/local/bin/aether`),
**medium** guide (`aether_for_llms_medium_contexts.md`, 51,303 b),
`--repair-attempts 2`, frontier trio (surface 15, algorithmic 14, spec 12).

Scores are filled in when the board completes; the design and its caveats are
recorded up front so they cannot be reconstructed wrongly later.

## Tiers

Tiers are parameter count × recency. Every model here has been benchmarked
before (see [`../history/HISTORY.md`](../history/HISTORY.md)), so this board
re-measures known subjects against the 1.0-era guide and compiler rather than
exploring new ones.

| tier | model | params | node | route |
|---|---|---|---|---|
| **high** | `deepseek-v4-flash-0731` | 284B / 13B active | claw1 (+claw2 rank 1) | **direct** `:8900` |
| **high** | `ornith-1.0-35b-nvfp4` | 35B NVFP4 | claw3 | T'Ra `claw3_ornith` |
| mid | `qwen/qwen3.6-35b-a3b` | 35B / 3B active | m5t | T'Ra `m5_remote` |
| mid | `zai-org/glm-4.7-flash` | MoE flash | m5t | T'Ra `m5_remote` |
| mid | `mistralai/devstral-small-2-2512` | 24B | m5t | T'Ra `m5_remote` |
| mid | `qwen3.6-27b-mlx-oq8` | 27B | m5t | T'Ra `m5_remote` |
| low | `qwen3.5-9b-mlx` | 9B | m5t | T'Ra `m5_remote` |
| low | `ornith-1.0-9b` | 9B | m5t | T'Ra `m5_remote` |
| low | `prism-coder-7b` | 7B | m5t | T'Ra `m5_remote` |
| low | `gemma-4-e4b-it-mlx@8bit` | ~4B effective | m5t | T'Ra `m5_remote` |
| low | `ibm/granite-4-h-tiny` | 7B | **m4t** *(see below)* | T'Ra `m2_remote` |

The high tier is the user's definition. `qwen3.5-122b-a10b` (m5t) would qualify
on parameter count alone and is the obvious third member if this board is
extended.

### The tiers above are wrong at the 35B boundary

`ornith-1.0-35b` and `qwen/qwen3.6-35b-a3b` are **the same class by every model
property this board tiers on**: both 35B MoE with ~3B active, and both report
architecture `qwen3_5_moe` — Ornith's `config.json` says
`Qwen3_5MoeForConditionalGeneration` outright. Ornith was placed in the high
tier and qwen3.6-35b-a3b in mid, but nothing about the *models* separates them.
Ornith's placement came from an operational fact (it has a dedicated claw
deployment), not a model one.

A caution on architecture strings: `qwen3_5_moe` is a family, not a generation.
Qwen **3.6** models report it too, so the string cannot by itself distinguish a
3.5-base from a 3.6-base, and the local metadata carries no `base_model` field.
If Ornith is a 3.5-generation derivative — plausible, and consistent with
everything observable here — then recency separates the pair even though size
and architecture do not, and the newer base winning is the expected result.

Measured: qwen3.6-35b-a3b **40/41**, ornith-1.0-35b **37/41**. Same size, same
architecture, so that gap is training, not capacity.

A tiering that survives this board would be:

| tier | basis | members |
|---|---|---|
| high | >100B total | `ds4` (284B/13B active) |
| mid | 24–35B | `qwen3.6-35b-a3b`, `ornith-1.0-35b`, `qwen3.6-27b`, `glm-4.7-flash`, `devstral-small-2` |
| low | ≤9B | `qwen3.5-9b`, `ornith-1.0-9b`, `granite-4.1-8b`, `prism-coder-7b`, `gemma-4-e4b`, `granite-4-h-tiny` |

That leaves one model in high, which is honest: on this fleet ds4 is the only
thing in its scale class, and the 35B group is the real top of the local range.
The `_tier` labels in the destinations config are left as they were run so the
board's rows stay traceable to the config that produced them.

## Lanes

Lanes are the physical serving nodes, not the tiers — m5t holds one LM Studio
model at a time, so its eight models queue while the two dedicated claw
deployments run concurrently.

| lane | node | destinations |
|---|---|---|
| A | claw1 | ds4 (+ a second spec pass at `--repeats 3`) |
| B | claw3 | ornith-35b |
| C | m5t | 4 mid + 4 low, sequential |
| D | m2t | granite-4-h-tiny |

## LM Studio federation: the T'Ra target is not the executing node

The three LM Studio nodes (m5t 128 GB, m4t 64 GB, m2t 32 GB) are **federated**,
and all of them advertise the same 127-model catalog. A T'Ra target therefore
selects the *entry point*, not the machine that runs the model — LM Studio
federation picks that, from whichever nodes physically hold the weights.

Measured, not assumed: `ibm/granite-4-h-tiny` was routed through T'Ra's
`m2_remote` and **executed on m4t**. `lms ps` reports the truth in its `DEVICE`
column (`Local` = m5t, `m4` = m4t, `M2-mini` = m2t); `lms ls` reports which
nodes hold each model, and a model may sit on several.

So "which node is this tier running on" cannot be read off the config. It has to
be read off `lms ps` while the model is loaded. This board's `_tier` labels are
about model size, and the lane names are about T'Ra targets — neither is a claim
about hardware.

Placement of this board's models, from `lms ls`:

| model | size | physically on |
|---|---:|---|
| `qwen3.5-9b-mlx`, `qwen3.6-27b-mlx-oq8` | 10.5 / 28.6 GB | Local only |
| `ornith-1.0-9b`, `prism-coder-7b`, `gemma-4-e4b@8bit` | 6.0 / 15.2 / 9.0 GB | **Local only** |
| `zai-org/glm-4.7-flash`, `devstral-small-2-2512` | 31.8 / 14.1 GB | Local **and** m4 |
| `ibm/granite-4-h-tiny` | 4.2 GB | Local **and** m4 → ran on m4 |

Models that exist only on `Local` are pinned to m5t by availability, which is
why the low-tier trio could not be moved off the laptop to relieve it.

## Caveats that shape what these numbers can say

**No retry-rate column except for ds4.** `--repeats` does not work through
T'Ra: the idempotency key returns one cached job N times instead of N samples.
Ten of the eleven models are therefore single-pass, and the runbook's own advice
— run discriminating suites with `--repeats 3` and report retry rate — can only
be honoured on the direct ds4 lane, where `..._spec_r3.json` holds the 3-repeat
spec run. Treat single-task differences between the other ten as noise; that is
this harness's documented behaviour, not caution for its own sake.

**ds4 is the one direct hit, deliberately.** Standing policy routes all
shared-GPU work through T'Ra, but ds4 is not a registered target and T'Ra
exposes no registration API (`GET /api/targets` only). T'Ra currently has no
healthy claw1/claw2 targets, so nothing is scheduled there to contend with. The
2026-08-10 frontier run used this same direct config. Registering ds4 properly
means editing the scheduler config on m4t and restarting it — worth doing before
the next board, not mid-run.

**Only glm-4.7-flash has a measured output bound.** Its `max_output_tokens` is
4096, a *runaway bound*: it prints the program, then the end marker, then recites
the guide (measured: marker at char 347 of 41,990). Every other T'Ra model is at
the standing 24000 precedent, generous on purpose because reasoning models
return empty content when the budget is tight. Any of them that recites after
answering will burn the full budget per task — visible as wall-clock, not as a
wrong score.

**The stop-token bug cannot fire on the T'Ra lanes.** T'Ra's `llm_generate`
payload forwards only model/preferred_targets/prompt/temperature/max_tokens, so
the harness's `__AETHER_BENCH_END__` stop never reaches those models. `stop:
null` is declared anyway — correct if the queue ever gains the field. On the
direct ds4 lane the stop is real and `stop: null` is load-bearing.

**Ornith moved nodes.** `destinations.tra.json` still points Ornith at
`claw1_ornith`, which is unhealthy — claw1 now serves ds4. This board targets
`claw3_ornith`. The stale entry in `destinations.tra.json` is untouched and will
mislead the next person who uses it.

## Run log

**Attempt 1 (06:41–07:00) — halted by a local network outage, not a fleet
problem.** Tailscale on this laptop logged itself out mid-run: the node lost its
session and could not re-authenticate because the network it is on resolves
`controlplane.tailscale.com` to `192.200.0.115`, which is not Tailscale. General
DNS and internet were unaffected throughout, which is why this looked at first
like T'Ra failing.

Sequence: ds4 finished `tasks_frontier` cleanly at 15/15 (424s) over the tunnel
while it was still up. Then `qwen3.6-35b-a3b` got 2/15 with 13 HTTP timeouts,
and every subsequent T'Ra run failed in ~30s. Lanes B and D never produced a
file.

| result | verdict |
|---|---|
| `high-ds4__tasks_frontier.json` | **kept** — 15/15 exact, 15/15 generated, 0 repairs |
| 5 files in `rejected/` | quarantined, will re-run |

**What the outage exposed.** The harness exits 0 and writes a well-formed report
even when the target is unreachable — either with zero variants (preflight skip)
or with real-looking scores where the failures are HTTP timeouts. The driver's
original check was `rc == 0 && file non-empty`, which accepted all of them. That
would have frozen `qwen3.6-35b-a3b` into this board at **2/15**, and
skip-if-exists would then have refused to re-run it: a network outage
permanently recorded as a capability score.

The driver now validates before accepting a result, and quarantines rather than
deletes:

- zero variants, zero cases, or `generated_ok == 0` → reject
- **any** case failing with a transport fingerprint (timeout, connection
  refused, unreachable, 502/503/504) → reject, regardless of count

The second rule is the load-bearing one. A model that writes bad Aether still
produced bytes; a model behind a dead socket produced nothing, and only the
first is a score. `gen_ok < total` with no transport errors is still accepted,
flagged `PARTIAL` — genuine weakness in a small model is a result, not an error.

Validated against all six real artifacts from attempt 1: the five outage files
reject, the ds4 file accepts.

## Reproducing

```bash
tmux new-session -d -s bench_tiers "caffeinate -i bash Tests/aether_doc_bench/bench_local_tiers_20260811.sh > Tests/aether_doc_bench/results/local_tiers_20260811/driver.log 2>&1"
```

Resumable: one file per destination-suite, skipped if present and non-empty,
written to `.tmp` and moved into place only on success — so a killed run never
leaves a truncated file that the skip check would honour. Re-running the command
picks up exactly where it stopped.

## Files

| file | what |
|---|---|
| `<destination>__<suite>.json` | one destination × one suite |
| `high-ds4__tasks_frontier_spec_r3.json` | the only 3-repeat run on the board |
| `logs/<destination>__<suite>.log` | harness stdout for that run |
| `logs/lane_[abcd]_*.log` | per-lane progress, `[run]`/`[done]`/`[FAIL]` |
| `driver.log` | preflight + lane orchestration |

## Toolchain drift mid-board (2026-08-11), and the pin

A separate session landed aether fixes while this board was running:
`/usr/local/bin/aether` was replaced at 10:53 (2026-08-09-1 -> 2026-08-11-1),
`VERSION` went to 2026-08-11-2, and the guides were edited more than once
(51,303 -> 51,618 -> 51,613 bytes within the hour).

Every result stamps `aether_version` and `doc_bytes`, so the damage is exactly
characterisable rather than suspected:

| rows | compiler | guide |
|---:|---|---|
| 13 | 2026-08-09-1 | 51,303 |
| 4 | mixed *(run spanned the 10:53 swap)* | 51,303 |
| 1 | 2026-08-11-1 | 51,303 |
| 1 | 2026-08-11-1 | 51,618 |

The four spanning rows are the subtle ones: a run that starts before the swap
and ends after it compiles early tasks with one binary and later tasks with
another, while stamping only the version it saw at startup. Timestamps, not
stamps, are what identify those.

**Measured impact, rather than assumed.** The landed fixes were an unknown-type
diagnostic (SCOPE-001) and tuple items that are arrays, `(Int, Int[])`. Scanning
every generated program on the board: **4 case-instances out of ~500** touch
those paths (`algo_knapsack_items`, `toon_fleet_rollup`,
`algo_union_find_components`, `algo_record_pipeline`), spread over 3 of 19 files,
and all four are already failures — so the only possible movement is fail->pass
by one or two tasks. Under 1%, inside this board's documented noise floor. The
rows were kept.

**The pin.** `Tests/aether_doc_bench/toolchain/aether-pinned` is a private copy
of the compiler, with its version in `PINNED_VERSION`; the drivers now gate
against that pin instead of the live `VERSION`, so the board holds its toolchain
still no matter what lands in `components/aether` next. The guide cannot be
pinned the same way -- `DOC_VARIANTS` is hardcoded in the harness, which the
other session is also editing -- so guide drift stays visible through `doc_bytes`
rather than prevented.

## The 35B head-to-head (rerun on the pinned toolchain)

`ornith-1.0-35b` and `qwen3.6-35b-a3b` are the only pair on this board with
identical size and architecture (35B MoE, ~3B active, `qwen3_5_moe`), which
makes their gap the cleanest capability signal available here — and the one most
easily ruined by toolchain drift, because the gap is small.

Their first runs were on aether 2026-08-09-1 + guide 2026-08-10-1 and scored
**37/41** and **40/41**. Both were re-run on the pinned aether 2026-08-11-1 +
guide 2026-08-11-3 so the comparison rests on one toolchain rather than two.

Ornith runs on **claw3** (`ornith-1.0-35b-nvfp4`, llama.cpp) — verified at launch
by `claw3_ornith` showing `active=ornith-1.0-35b-nvfp4`. That matters because m5t
holds a *different* Ornith 35B (`ornith-1.0-35b-mlx`, 5-bit, 25 GB); the two are
not interchangeable and federation must not be allowed to substitute one for the
other. qwen3.6-35b-a3b runs on m5t as before.

The superseded first runs are kept in `superseded/` rather than deleted — they
are the only record of these models on the earlier toolchain, and the delta
across toolchains is itself worth having.
