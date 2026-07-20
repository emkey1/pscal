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
| 4 | qwen35-9b-4bit-cs-aug18 | running | no | |
| 5 | qwen35-9b-8bit-cs-aug18 | queued | no | should train clean first-try -- 8-bit export fix deployed before this run started |
| 6 | qwen35-9b-16bit-cs-aug18 | queued | no | |

Status values: queued / running / done / FAILED. Benchmarked: no / yes (simple X/30,
large X/8, cs X/19).

Check live queue state:
```bash
ssh claw@claw2 "tail -40 /storage/aether_retrain_queue_cs_aug18_precision/queue.log"
```
