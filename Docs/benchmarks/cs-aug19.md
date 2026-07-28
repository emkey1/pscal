# cs-aug19 - 8B/9B precision grid

*Corpus:* cs-aug19 (723 instruction + 38 repair)  
*Host:* claw3  
*Repair attempts:* 0 (harness default; not passed explicitly)  
*Grader:* aether Aether Compiler Version: 2026-07-20-1 (latest tag: untagged)  
*Task-suite versions:* simple=2026-07-15-1, large=2026-07-15-1, cs=2026-06-23-1  
*Models:* 6

## Results

| model | simple (35) | large (9) | cs (19) |
|---|---|---|---|
| `qwen3-8b-nothink-16bit-cs-aug19` | **28** | **4** | **9** |
| `qwen3-8b-nothink-4bit-cs-aug19` | **27** | **3** | **10** |
| `qwen3-8b-nothink-8bit-cs-aug19` | **28** | **4** | **10** |
| `qwen35-9b-16bit-cs-aug19` | **24** | **1** | **8** |
| `qwen35-9b-4bit-cs-aug19` | **24** | **4** | **8** |
| `qwen35-9b-8bit-cs-aug19` | **30** | **4** | **9** |

Single score per task, majority of repeats. This run had no repair attempts, so it is directly comparable to other repair-0 boards.

## Integrity

- `qwen3-8b-nothink-16bit-cs-aug19` / cs: `generated_ok` 54/57 - empty or failed generations; treat that cell as a floor.
- `qwen3-8b-nothink-8bit-cs-aug19` / large: `generated_ok` 24/27 - empty or failed generations; treat that cell as a floor.
- `qwen3-8b-nothink-8bit-cs-aug19` / cs: `generated_ok` 51/57 - empty or failed generations; treat that cell as a floor.

## Notes

- Baseline board for the 0-based `Text` work that followed. Six models: two families x 4/8/16-bit.
- 8-bit was the strongest arm in both families, replicating cs-aug18. That finding was later retired -- see cs-aug20-small-repair2, where enabling repair dissolves the precision spread.
