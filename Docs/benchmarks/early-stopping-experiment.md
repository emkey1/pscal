# Early-stopping experiment - qwen3-8b-nothink-8bit

*Corpus:* cs-aug20 (689 instruction after a 75-case holdout)  
*Host:* claw2  
*Repair attempts:* 2  
*Grader:* aether Aether Compiler Version: 2026-07-20-1 (latest tag: untagged)  
*Task-suite versions:* simple=2026-07-15-1, large=2026-07-15-1, cs=2026-06-23-1  
*Models:* 1

## Results

| model | simple (35) | large (9) | cs (19) |
|---|---|---|---|
| `qwen3-8b-nothink-8bit-cs-aug20-es2` | 27 -> **30** | 6 -> **7** | 12 -> **13** |

Cells are *first attempt* -> **after repair**. Per-task scoring is majority of repeats.

## Integrity

`generated_ok` is complete for every model and suite in this run.

## Notes

- Single-model test of whether the early-stopping criterion was ending runs prematurely. The default is a 12-sample eval set checked every 5 steps with `early_stopping_threshold=0.0` and patience 2, so a 0.03% uptick stops training.
- Treatment (75-case holdout, threshold 0.001, patience 4, eval every 20 steps) ran to epoch 2.77 instead of the control's 1.70 -- and scored the same. With a resolvable eval set the true optimum is ~epoch 1.85 and eval_loss genuinely rises after it, and `load_best_model_at_end` exports a near-best checkpoint either way.
- Conclusion: the criterion is noisy but not systematically costly. It does not invalidate any board. Caveat: the treatment also trained on 8% fewer records because of the larger holdout.
