# cs-aug20 - qwen35-9b family (first pass, repair-0)

*Corpus:* cs-aug20 (726 instruction + 38 repair)  
*Host:* claw3  
*Repair attempts:* 0 (harness default)  
*Grader:* aether Aether Compiler Version: 2026-07-20-1 (latest tag: untagged)  
*Task-suite versions:* simple=2026-07-15-1, large=2026-07-15-1, cs=2026-06-23-1  
*Models:* 3

## Results

| model | simple (35) | large (9) | cs (19) |
|---|---|---|---|
| `qwen35-9b-16bit-cs-aug20` | **29** | **1** | **11** |
| `qwen35-9b-4bit-cs-aug20` | **30** | **1** | **12** |
| `qwen35-9b-8bit-cs-aug20` | **32** | **6** | **11** |

Single score per task, majority of repeats. This run had no repair attempts, so it is directly comparable to other repair-0 boards.

## Integrity

- `qwen35-9b-16bit-cs-aug20` / cs: `generated_ok` 54/57 - empty or failed generations; treat that cell as a floor.

## Notes

- Re-run after all three 9B models failed to serve at `--gpu-memory-utilization 0.18`. The 9B needs 0.22: 17.66 GiB of weights against a ~21.5 GiB budget leaves too little for KV cache, and the failure surfaces only as `Engine core initialization failed`.
- That constant is model-size- and host-specific, not a property of the box.
