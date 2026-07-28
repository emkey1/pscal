# cs-aug20 - 14B-35B roster

*Corpus:* cs-aug20 (726 instruction + 38 repair)  
*Host:* claw2  
*Repair attempts:* 2  
*Grader:* aether Aether Compiler Version: 2026-07-20-1 (latest tag: untagged)  
*Task-suite versions:* simple=2026-07-15-1, large=2026-07-15-1, cs=2026-06-23-1  
*Models:* 5

## Results

| model | simple (35) | large (9) | cs (19) |
|---|---|---|---|
| `mistral24b-cs-aug20` | 30 -> **34** | 1 -> **7** | 13 -> **14** |
| `qwen25-14b-cs-aug20` | 28 -> **33** | 1 -> **4** | 14 -> **15** |
| `qwen3-coder30b-a3b-cs-aug20` | 26 -> **28** | 6 -> **8** | 15 -> **16** |
| `qwen36-27b-cs-aug20` | 29 -> **32** | 6 -> **6** | 17 -> **19** |
| `qwen36-35b-a3b-cs-aug20` | 32 -> **34** | 7 -> **7** | 18 -> **19** |

Cells are *first attempt* -> **after repair**. Per-task scoring is majority of repeats.

## Integrity

`generated_ok` is complete for every model and suite in this run.

## Notes

- First board for the 14B-35B tier on this corpus. Served on claw2 at `--gpu-memory-utilization 0.85`; these merges are 28-67 GB and do not fit claw3's usable window at all.
- `mistral24b` was served with the base tokenizer. The Unsloth merge omits `tekken.json`, so vLLM falls back to the HF path and drops spaces in generated text; it scored 9/35 on `simple` until corrected, then 34/35 on identical weights. Note `tokenizer.json` is byte-identical to base, so comparing that file does not detect the problem.
- Two models reached a perfect 19/19 on `cs`.
