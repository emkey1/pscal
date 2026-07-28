# cs-aug20 - 8B/9B grid re-scored with repair

*Corpus:* cs-aug20 (726 instruction + 38 repair)  
*Host:* claw3  
*Repair attempts:* 2  
*Grader:* aether Aether Compiler Version: 2026-07-20-1 (latest tag: untagged)  
*Task-suite versions:* simple=2026-07-15-1, large=2026-07-15-1, cs=2026-06-23-1  
*Models:* 6

## Results

| model | simple (35) | large (9) | cs (19) |
|---|---|---|---|
| `qwen3-8b-nothink-16bit-cs-aug20` | 25 -> **27** | 3 -> **6** | 12 -> **15** |
| `qwen3-8b-nothink-4bit-cs-aug20` | 27 -> **33** | 2 -> **4** | 11 -> **14** |
| `qwen3-8b-nothink-8bit-cs-aug20` | 30 -> **30** | 6 -> **7** | 13 -> **14** |
| `qwen35-9b-16bit-cs-aug20` | 29 -> **32** | 1 -> **2** | 11 -> **11** |
| `qwen35-9b-4bit-cs-aug20` | 30 -> **33** | 1 -> **4** | 12 -> **13** |
| `qwen35-9b-8bit-cs-aug20` | 32 -> **32** | 6 -> **8** | 11 -> **11** |

Cells are *first attempt* -> **after repair**. Per-task scoring is majority of repeats.

## Integrity

- `qwen35-9b-16bit-cs-aug20` / cs: `generated_ok` 54/57 - empty or failed generations; treat that cell as a floor.

## Notes

- The 8B/9B grid re-run with `--repair-attempts 2` so both tiers of the board sit on the same setting. The harness defaults this to 0 while cs-aug4 used 1, which had quietly given the older baseline a free extra attempt per failure.
- Every first-attempt score here reproduced its repair-0 original exactly, 6 for 6 -- generation is reproducible across runs and hosts.
- Repair compresses the precision spread: `cs` goes 11/13/12 to 14/14/15 in the 8B family. The earlier '8-bit always wins' result was substantially an artifact of repair-0 scoring.
