# cs-aug18 precision grid — running status

Scratch tracker for the 6-run 2x3 (model x precision) grid. See
`cs-aug18-precision-grid.md` for the full recipe. Updated by hand as the queue
progresses; not committed on every edit, just kept current for session-resume legibility.

Queue: `retrain_queue_cs_aug18_precision.sh` on claw2, sequential (single GPU),
launched 2026-07-19.

| # | tag | training | benchmarked | notes |
|---|---|---|---|---|
| 1 | qwen3-8b-nothink-4bit-cs-aug18 | done | **yes: 29/35 simple, 4/9 large, 11/19 cs** | merged export OK, 5 shards, ~15min; stopped early at step ~65/255 (epoch 0.77) -- same absolute step count as this model/precision's cs-aug4 stopping point |
| 2 | qwen3-8b-nothink-8bit-cs-aug18 | done (recovered) | **yes: 32/35 simple, 7/9 large, 10/19 cs** | training succeeded (eval_loss ~0.122) but queue's auto-export crashed on an Unsloth 8-bit bug (get_loading_attributes lambda not JSON-serializable, see unsloth_qwen_coder_30b_sft.py commit); fixed + re-exported from the saved adapter via --adapter-dir, no retrain needed. First bench attempt got 0/0/0 "connection refused" (vLLM OOM'd, claw2 rebooted to clear a memory leak); retry succeeded cleanly. Beats 4-bit on simple (+3) and large (+3), loses slightly on cs (-1) |
| 3 | qwen3-8b-nothink-16bit-cs-aug18 | done | **yes: 30/35 simple, 3/9 large, 9/19 cs** | merged export OK, 5 shards. Lowest of the three precisions on large and cs, middling on simple -- qwen3-8b-nothink family complete: 4bit=(29,4,11), 8bit=(32,7,10), 16bit=(30,3,9). No monotonic precision ordering, matches the original board's "not a clean win either way" |
| 4 | qwen35-9b-4bit-cs-aug18 | done (recovered) | **yes (clean): 26/35 simple, 1/9 large, 7/19 cs** | training succeeded (eval_loss ~0.107) but hit a second export bug: my first get_loading_attributes fix used vars(quant_config), which crashes for 4-bit's dict-typed quantization_config (no __dict__); fixed to hasattr()/delattr(), re-exported from saved adapter, merged_16bit confirmed (4 shards). First bench attempt (19/35, 8/35 repairs) was CONTAMINATED -- model generated with thinking enabled (raw output began "Thinking Process:..."), fed to compiler as source. Root cause: destination config used a top-level "chat_template_kwargs" key aether_doc_bench.py's openai_chat_completions builder never reads -- needed "extra_body": {"chat_template_kwargs": {...}}. Fixed + verified via direct curl before retrying; clean rerun jumped to 26/35 with 0 repairs (confirms diagnosis). Notably weak on large (1/9), well below the whole qwen3-8b-nothink family |
| 5 | qwen35-9b-8bit-cs-aug18 | done (checkpoint-resumed) | **yes: 32/35 simple, 6/9 large, 9/19 cs** | 2nd claw2 crash mid-retrain at step ~40 (checkpoint-40 corrupt/zero-byte, checkpoint-35 intact); added find_resumable_checkpoint() + resume_from_checkpoint plumbing, removed corrupt checkpoint-40, resumed from step 35/255 -- finished, eval_loss ~0.101, merged export confirmed (4 shards). Clean bench run (thinking-disable fix already in place). Much stronger than 4-bit across all 3 suites (26/35, 1/9, 7/19) -- bigger 4bit-vs-8bit gap than qwen3-8b-nothink showed, suggesting qwen35-9b may be more precision-sensitive at 4-bit than the older architecture |
| 6 | qwen35-9b-16bit-cs-aug18 | done | **yes: 29/35 simple, 2/9 large, 7/19 cs** | first attempt crashed instantly at get_peft_model() (torchao dispatcher bug, see commit history); retry succeeded, merged export confirmed (4 shards). qwen35-9b family complete: 4bit=(26,1,7), 8bit=(32,6,9), 16bit=(29,2,7). 16bit clearly below 8bit on large (2 vs 6) and cs (7 vs 9) -- matches the qwen3-8b-nothink family's own 16bit-underperforms-8bit-on-hard-suites pattern, 2-for-2 across independent architectures |

**All 6 training runs AND all 18 benchmark suites complete as of 2026-07-20T17:44Z.**

## Final 2x3 grid (exact_stdout_match / total_cases)

| model | precision | simple (35) | large (9) | cs (19) |
|---|---|---|---|---|
| qwen3-8b-nothink | 4-bit | 29 | 4 | 11 |
| qwen3-8b-nothink | 8-bit | 32 | 7 | 10 |
| qwen3-8b-nothink | 16-bit | 30 | 3 | 9 |
| qwen35-9b | 4-bit | 26 | 1 | 7 |
| qwen35-9b | 8-bit | 32 | 6 | 9 |
| qwen35-9b | 16-bit | 29 | 2 | 7 |

Both families: 8-bit is the best precision on every suite except qwen3-8b-nothink's cs
(where 4-bit edges 8-bit by 1, within noise per SE~11pp on n=19). 16-bit is NOT the
strongest precision on the harder suites in either family -- it's the weakest or
near-weakest on large for both (3/9 and 2/9), consistent with the original board's
documented overfitting-to-small-corpus finding for bf16/16bit, now replicated on a
~55%-larger corpus and a second independent model family.

**2026-07-20T01:55Z: claw2 powered off mid-run-5** (known intermittent issue per user, recurred a few days back then was stable). Runs 1-4's merged exports survived on persistent /storage, confirmed intact after claw2 came back ~2026-07-20T02:04Z. Queue resumed automatically (resumable design, skips any tag with an existing merged_16bit) -- no manual recovery needed for 1-4, only run 5 restarted from scratch.

Status values: queued / running / done / FAILED. Benchmarked: no / yes (simple X/30,
large X/8, cs X/19).

Check live queue state:
```bash
ssh claw@claw2 "tail -40 /storage/aether_retrain_queue_cs_aug18_precision/queue.log"
```
