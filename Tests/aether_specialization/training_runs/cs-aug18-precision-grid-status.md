# cs-aug18 precision grid — running status

Scratch tracker for the 6-run 2x3 (model x precision) grid. See
`cs-aug18-precision-grid.md` for the full recipe. Updated by hand as the queue
progresses; not committed on every edit, just kept current for session-resume legibility.

Queue: `retrain_queue_cs_aug18_precision.sh` on claw2, sequential (single GPU),
launched 2026-07-19.

| # | tag | training | benchmarked | notes |
|---|---|---|---|---|
| 1 | qwen3-8b-nothink-4bit-cs-aug18 | done | no | merged export OK, 5 shards, ~15min |
| 2 | qwen3-8b-nothink-8bit-cs-aug18 | done (recovered) | no | training succeeded (eval_loss ~0.122) but queue's auto-export crashed on an Unsloth 8-bit bug (get_loading_attributes lambda not JSON-serializable, see unsloth_qwen_coder_30b_sft.py commit); fixed + re-exported from the saved adapter via --adapter-dir, no retrain needed |
| 3 | qwen3-8b-nothink-16bit-cs-aug18 | done | no | merged export OK, 5 shards |
| 4 | qwen35-9b-4bit-cs-aug18 | done (recovered) | no | training succeeded (eval_loss ~0.107) but hit a second export bug: my first get_loading_attributes fix used vars(quant_config), which crashes for 4-bit's dict-typed quantization_config (no __dict__); fixed to hasattr()/delattr(), re-exported from saved adapter, merged_16bit confirmed (4 shards) |
| 5 | qwen35-9b-8bit-cs-aug18 | done (checkpoint-resumed) | no | 2nd claw2 crash mid-retrain at step ~40 (checkpoint-40 corrupt/zero-byte, checkpoint-35 intact); added find_resumable_checkpoint() + resume_from_checkpoint plumbing, removed corrupt checkpoint-40, resumed from step 35/255 -- finished, eval_loss ~0.101, merged export confirmed (4 shards) |
| 6 | qwen35-9b-16bit-cs-aug18 | running | no | queue script relaunched (its own bash process died with the 3rd crash), correctly skipped 1-5 via merged_16bit check |

**2026-07-20T01:55Z: claw2 powered off mid-run-5** (known intermittent issue per user, recurred a few days back then was stable). Runs 1-4's merged exports survived on persistent /storage, confirmed intact after claw2 came back ~2026-07-20T02:04Z. Queue resumed automatically (resumable design, skips any tag with an existing merged_16bit) -- no manual recovery needed for 1-4, only run 5 restarted from scratch.

Status values: queued / running / done / FAILED. Benchmarked: no / yes (simple X/30,
large X/8, cs X/19).

Check live queue state:
```bash
ssh claw@claw2 "tail -40 /storage/aether_retrain_queue_cs_aug18_precision/queue.log"
```
