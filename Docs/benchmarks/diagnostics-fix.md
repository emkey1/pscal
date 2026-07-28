# Diagnostic fixes re-measured - 14B-35B roster

*Corpus:* cs-aug20 (726 instruction + 38 repair)  
*Host:* claw2  
*Repair attempts:* 2  
*Grader:* aether Aether Compiler Version: 2026-07-26-9 (latest tag: untagged)  
*Task-suite versions:* simple=2026-07-15-1, large=2026-07-15-1, cs=2026-06-23-1, nontoon=2026-07-27-1  
*Models:* 3  
*Status:* PARTIAL - some models/suites missing

## Results

| model | simple (35) | large (9) | cs (19) | nontoon (5) |
|---|---|---|---|---|
| `mistral24b-cs-aug20` | 30 -> **34** | 1 -> **8** | 13 -> **14** | 1 -> **2** |
| `qwen25-14b-cs-aug20` | 28 -> **33** | 1 -> **4** | 14 -> **15** | 1 -> **2** |
| `qwen36-27b-cs-aug20` | 29 -> **32** | 6 -> **7** | - | 1 -> **2** |

Cells are *first attempt* -> **after repair**. Per-task scoring is majority of repeats.

## Integrity

- `qwen36-27b-cs-aug20` / nontoon: `generated_ok` 12/15 - empty or failed generations; treat that cell as a floor.

## Notes

- Re-measurement after four misleading compiler diagnostics were fixed: wrong-arity builtin calls reported as `identifier not in scope`, builtin redefinition reported as `expected parameter name`, raw `Yyjson*` internals surfacing to users, and the 1-D-array-indexed-as-2-D VM error.
- The grader is deliberately the FIXED compiler, so the compiler is the intended variable against the earlier board.
- This run also carries the first `nontoon` numbers, from the new non-TOON hard suite.
- Result: the diagnostics are provably gone (`has_toon` SYN-001 27 -> 0, raw `Yyjson*` -> 0) and `large` barely moved. The bad messages were masking ordinary model errors -- type errors, missing returns, missing `fx` blocks -- rather than causing failures. Recorded as a null result.
