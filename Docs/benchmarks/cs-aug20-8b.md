# cs-aug20 - qwen3-8b family (first pass, repair-0)

*Corpus:* cs-aug20 (726 instruction + 38 repair)  
*Host:* claw3  
*Repair attempts:* 0 (harness default)  
*Grader:* aether Aether Compiler Version: 2026-07-20-1 (latest tag: untagged)  
*Task-suite versions:* simple=2026-07-15-1, large=2026-07-15-1, cs=2026-06-23-1  
*Models:* 3

## Results

| model | simple (35) | large (9) | cs (19) |
|---|---|---|---|
| `qwen3-8b-nothink-16bit-cs-aug20` | **25** | **3** | **12** |
| `qwen3-8b-nothink-4bit-cs-aug20` | **27** | **2** | **11** |
| `qwen3-8b-nothink-8bit-cs-aug20` | **30** | **6** | **13** |

Single score per task, majority of repeats. This run had no repair attempts, so it is directly comparable to other repair-0 boards.

## Integrity

`generated_ok` is complete for every model and suite in this run.

## Notes

- First board on the corpus with the 0-based `Text` migration finished and two inverted repair pairs corrected.
- `cs` rose in all three arms. `simple` and `large` moved inconsistently.
