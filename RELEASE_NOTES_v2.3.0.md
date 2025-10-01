# Pscal v2.3.0

Date: 2025-10-01

## Highlights
- Extended built-ins now bundle sqlite and yyjson support with CLI and runtime introspection so programs can detect optional capabilities.
- Landscape and Mandelbrot demos ship fast render paths by default, complete with validation fallbacks, refreshed visuals, and new graphics built-ins.
- Developer tooling reorganized around `Tests/libs`, dedicated front-end runners, and a hand-maintained Xcode project that mirrors CMake targets.
- Pscal now distributes under the MIT license, aligning with new third-party components and simplifying downstream redistribution.

## New
- Extended built-ins
  - Added the sqlite category with connection, statement, metadata, and binding helpers that work uniformly across Pascal, CLike, and Rea.
  - Added the yyjson category, bundling the upstream parser and exposing document/query helpers plus smoke coverage for every front end.
  - Introduced `--dump-ext-builtins` on all binaries and new introspection routines (`ExtBuiltinCategoryCount`, `HasExtBuiltin`, etc.) so programs and tests can enumerate active categories, groups, and functions.
  - Added a `RealTimeClock` helper to the system category and ensured builtin identifiers remain stable even when optional modules are disabled.
- Graphics & demos
  - Implemented a fast SDL landscape rendering pipeline with sky refreshes, improved sun/cloud shading, and water level configuration; the renderer now validates incoming terrain descriptors and falls back to the safe path when fast draw support is missing.
  - Enabled the fast draw path by default for Rea landscape demos and surfaced runtime fallbacks when extended built-ins are omitted.
  - Registered a `glCullFace` builtin for the CLike front end and documented its usage in the graphics reference.
  - Optimized the SDL Mandelbrot example with multithreaded scheduling and tuned constants for smoother rendering on constrained devices.
- Languages & runtime
  - Rea's lexer/parser now accepts `match` identifiers, refines comment heuristics, and restores context-aware `//` handling so inline comments survive integer-division fixes.
  - Pascal's address-of operator works on array elements, and byte arguments now coerce safely when invoking integer-only builtins.
  - CRT helpers track text attributes for the CLike runtime, enabling color-preserving libraries and test fixtures.
  - Front ends share a conditional preprocessor so `#ifdef` checks can test for extended builtin categories at compile time.
- Tooling & IDE
  - Examples reorganized into `Examples/{pascal,rea}/{base,sdl}` families with updated assets and bake steps for vertex data.
  - Added a hand-maintained Xcode project and generator (`xcode/`) with configurable run schemes for Pascal, CLike, and Rea binaries.
  - Documented the scope and library harnesses, updated AGENTS for the new layouts, and added explicit scripts for exercising each library suite.

## Improvements
- HTTP helpers tolerate transport failures, cancellation, and throttled downloads; Pascal runtime warnings now log without aborting executions.
- GraphLoop accepts real-valued delays, validates inputs, and exposes the same behavior across SDL back ends and Rea demos.
- Module caches handle growing import lists without invalidating or leaking state, and extended builtin inputs are verified before reaching the renderer.
- Library test harnesses standardize gray output across Pascal and CLike suites, skip timeout wrappers on Alpine, and gained dedicated driver scripts.
- SDL demos refine plasma color ramps, bake landscape vertices before rendering, and correct control bindings and font packaging for bundled games.

## Fixed
- Resolved compiler crashes triggered by byte-to-integer parameter coercions and kept builtin ID assignments consistent across optional builds.
- Landscape demos clamp terrain min/max parameters, recompute MVP matrices correctly, and repair movement controls introduced during refactors.
- Rea scope and comment handling now keeps identifiers in `match` expressions, respects inline comments, and improves module cache growth handling.
- Block game and landscape samples restore `OutTextXY` usage, active piece rendering, and water level initialization.
- Weather JSON demos free HTTP resources, normalize UNIX timestamp formatting, and round temperatures before display.

## Build & Install
- Configure with `cmake -S . -B build [-DSDL=ON] [-DBUILD_DASCAL=ON] [-DENABLE_EXT_BUILTIN_SQLITE=ON/OFF] [-DENABLE_EXT_BUILTIN_YYJSON=ON/OFF]`.
- Build via `cmake --build build -j` and install with `cmake --install build` (installs front ends, VM, decompiler, JSON compiler, and optional completions).
- The Xcode project in `xcode/` mirrors these targets and exposes configurable runners for rapid iteration without CMake regeneration.

## Testing
- Use `Tests/run_pascal_tests.sh`, `Tests/run_clike_tests.sh`, and `Tests/run_rea_tests.sh` for targeted library suites; `Tests/run_all_suites.py` aggregates them when needed.
- SDL tests default to dummy drivers; set `RUN_SDL=1` for real hardware. Network suites remain behind `RUN_NET_TESTS=1` to preserve offline runs.
- Extended builtin enumeration and sqlite/yyjson smoke tests honor `REA_TEST_EXT_BUILTINS` and `REA_TEST_HAS_YYJSON` in environments without optional modules.

## Compatibility
- VM bytecode cache version remains `PSCAL_VM_VERSION = 7`; bytecode from v2.21 continues to load without recompilation.
- sqlite support expects an available system SQLite3 library; disable the category when packaging for targets without it.
- The bundled yyjson library carries an MIT license, matching the project's updated licensing.
- `--dump-ext-builtins` emits a line-oriented inventory consumed by the new harnesses; keep scripts in sync if you alter its format.

## Known Notes
- Fast landscape rendering depends on the graphics extended built-ins; when disabled, the demo automatically falls back to the previous renderer.
- SDL graphics still require real video/audio drivers when dummy backends are not configured.
