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
| low | `ibm/granite-4-h-tiny` | tiny | m2t | T'Ra `m2_remote` |

The high tier is the user's definition. `qwen3.5-122b-a10b` (m5t) would qualify
on parameter count alone and is the obvious third member if this board is
extended.

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
