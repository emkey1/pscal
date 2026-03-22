# Pscal v4.0.0

Date: 2026-03-22

## Highlights
- **Pascal grows into a much broader modern dialect** – The Pascal front end now supports a substantial set of Delphi-style conveniences and PSCAL-specific extensions, including inline `var`, `for var`, `with`, `try/except`, `raise`, `Exit(value)`, `continue`, record constructors, richer variant records, and selector chains on call results.
- **Pascal runtime and VM behavior are significantly stronger** – Function-result handling for records/pointers is safer, `StringOfChar` and Unicode-friendly `Write`/`Writeln` support land, inline-closure and inline-parameter lowering is hardened, and interface dispatch now builds interface-specific method tables so virtual calls resolve correctly even when non-interface methods appear earlier on the record.
- **Examples are now a major strength of the tree** – The Pascal suite adds or substantially upgrades checkers, 3D checkers, Game of Life, Eight Puzzle, Turing Machine, Web Crawler, Prolog, Lambda Calculus, directory browsing, and puzzle-solving demos.
- **SDL and documentation quality both improve** – The SDL checkers demos were overhauled for correctness and usability, while Smallclue’s Markdown rendering and multiple Pascal-language/reference documents were updated to match the larger feature set.

## New
- Added Pascal support for:
  - inline `var` declarations in compound blocks and the main program body
  - `for var ... := ... do` loop-local variables
  - `with ... do`
  - `try ... except ... end` and `raise`
  - `Exit(value)` in functions
  - `continue`
  - Delphi-style named record constructors
  - untagged and overlapping-storage variant records
  - postfix selectors on call results such as `GetAllMoves(...)[i]`
- Added `StringOfChar` as a Pascal-visible builtin and documented it alongside the existing VM builtins.
- Added Unicode-aware `Write`/`Writeln` output handling so valid UTF-8 strings and legacy 8-bit text output display correctly.
- Added new Pascal compiler regressions for:
  - record function results
  - inline parameter shadowing
  - inline `Exit(...)` inside inlined helpers
  - inline record value parameters
  - anonymous record globals
  - interface virtual-slot ordering
  - variant-record overlap and pointer-result access
  - `StringOfChar`
  - `Val(...)` var-argument handling
  - `continue`
- Added or substantially expanded example programs:
  - `Examples/pascal/base/GameOfLife`
  - `Examples/pascal/base/TuringMachine`
  - `Examples/pascal/base/EightPuzzle`
  - `Examples/pascal/base/WebCrawler`
  - `Examples/pascal/base/PrologEngine`
  - `Examples/pascal/base/LamdaCalc`
  - `Examples/pascal/base/DirBrowser`
  - `Examples/pascal/sdl/CheckersSDL`
  - `Examples/pascal/sdl/Checkers3D`

## Improvements
- Pascal interface dispatch now builds method tables per interface at boxing time, preventing non-interface methods from corrupting interface slot order.
- Variant-record storage and field aliasing were strengthened so overlapping fields share storage correctly and function-result field access works through pointers.
- The Pascal front end better supports modern sample style: loop-local variables, inline declarations, nested closures, richer exception flow, and more expressive statement forms now compose reliably.
- `GameOfLife` now redraws in-place, supports pattern save/load, and no longer emits repeated `UNKNOWN_VAR_TYPE` noise during normal operation.
- `TuringMachine` is now an interactive simulator with built-in machines, file-driven transition tables, controls, and dedicated documentation in [turing_machine_example.md](Docs/turing_machine_example.md).
- `EightPuzzle` now generates random solvable scrambles, prints the move sequence and replay, and uses improved seeding so consecutive launches are less repetitive.
- `WebCrawler` was expanded from a toy queue demo into a more realistic threaded crawler example with crawling limits, filtering, content summaries, and local `file://` verification.
- `PrologEngine` now includes more explanatory comments and serves as a clearer demonstration of recursive unification and backtracking.
- Smallclue’s Markdown reader now respects terminal width better and no longer misclassifies wrapped prose as selector noise in documents such as the Turing Machine guide.

## Fixed
- Fixed Pascal inline-routine lowering bugs that could corrupt locals or mishandle `Exit(...)` inside inlined helpers.
- Fixed Pascal global anonymous-record metadata so field lookups on globals work at runtime.
- Fixed record-result initialization so record and pointer function results can be populated safely before return.
- Fixed interface dispatch misrouting when a record had non-virtual methods ahead of the virtual interface method.
- Fixed `StringOfChar` lookup failures and `Val(...)`-related crashes that surfaced in Pascal examples.
- Fixed multiple SDL Checkers issues:
  - AI path correctness after chained captures and rule changes
  - optional-vs-forced capture handling
  - multi-jump continuation
  - input focus and event-loop stalls
  - blank-window rendering caused by mixed GL/2D state
  - board centering and 3D framing issues
- Fixed `GameOfLife` edit-mode parsing, nested-array execution problems, and load/save stability issues.
- Fixed `PuzzleSolver`, `DirBrowser`, `WebCrawler`, `TuringMachine`, and other examples that previously used unsupported or invalid source patterns.

## Upgrade Notes
- Rebuild with `cmake -S . -B build -DRELEASE_BUILD=ON` and `cmake --build build` for release packaging, or use `-Drelease=ON` if you want the PSCAL release profile enabled.
- The current VM bytecode cache version is `PSCAL_VM_VERSION = 9`. Cached bytecode built against older VM/interface metadata should be considered stale and recompiled.
- Pascal developers should review the updated language reference for the expanded dialect surface, especially around exception handling, inline declarations, record constructors, variant records, and `with`.
- Several Pascal examples now rely on SDL or other optional runtime capabilities; enable those dependencies before performing full release validation of the example suite.

## Verification Checklist
1. `cmake -S . -B build && cmake --build build`
2. `Tests/run_all_tests`  
   Optional: export `RUN_SDL=1` and `RUN_NET_TESTS=1` for broader release sweeps.
3. `python3 Tests/compiler/pascal/run_compiler_tests.py`
4. Spot-check key Pascal examples:
   - `build/bin/pascal Examples/pascal/base/GameOfLife`
   - `build/bin/pascal Examples/pascal/base/TuringMachine`
   - `build/bin/pascal Examples/pascal/base/EightPuzzle`
   - `build/bin/pascal Examples/pascal/base/WebCrawler`
   - `build/bin/pascal Examples/pascal/base/PrologEngine`
5. Spot-check SDL demos when SDL is enabled:
   - `build/bin/pascal Examples/pascal/sdl/CheckersSDL`
   - `build/bin/pascal Examples/pascal/sdl/Checkers3D`
