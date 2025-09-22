# Repository Guidelines

## Project Structure & Module Organization
The runtime and front ends live under `src/`, grouped by subsystem: `src/core` (runtime utilities), `src/Pascal` (Pascal compiler), `src/clike` (C-like front end), `src/vm` (bytecode VM), and `src/tools` for shared helpers. Generated build artifacts belong in `build/` (ignored by Git). `Tests/` hosts regression suites with front-end-specific folders (`Tests/Pascal`, `Tests/clike`, `Tests/tiny`, `Tests/rea`). `Examples/` contains runnable demos such as `Examples/Pascal/ThreadsProcPtrDemo`. `Docs/` captures design notes and tutorials; `lib/` ships bundled runtime assets (for example `lib/misc/simple_web_server/htdocs`). CLI utilities, scripts, and the Tiny language front end live in `tools/`.

## Build, Test, and Development Commands
- `cmake -S . -B build [-DSDL=ON] [-DBUILD_DASCAL=ON]` configures the project; SDL toggles graphics/audio support.
- `cmake --build build` compiles all binaries into `build/bin/`.
- `cmake --build build --target run_threads_procptr_demo` launches the threaded Pascal demo after a build.
- `Tests/run_all_tests` runs the aggregated regression sweep; set `RUN_SDL=1` to include SDL cases.
- Targeted suites: `Tests/run_pascal_tests.sh`, `Tests/run_clike_tests.sh`, `Tests/run_tiny_tests.sh`, and `Tests/run_rea_tests.sh`; network-sensitive demos need `RUN_NET_TESTS=1`.

## Coding Style & Naming Conventions
Adhere to the C11 style in-tree: four-space indentation, braces on the same line as declarations, and camelCase for functions (`varTypeToString`) with uppercase `snake_case` macros and include guards (`UTILS_H`). Keep source files in lowercase directories while Pascal units retain initial capitals. Avoid compiler warnings, order includes from project headers to system headers, and ensure helper scripts remain POSIX-sh compatible.

## Testing Guidelines
Regression tests pair a source file with expected `.out` or `.err` artifacts (see `Tests/Pascal/ApiSendReceiveTest*`). Add coverage in the matching front end directory and mirror the `FeatureName`/`FeatureName.out` naming. Update auxiliary fixtures under `Tests/tools/` when needed and run both the targeted suite and `Tests/run_all_tests` before pushing. Document `RUN_SDL` or `RUN_NET_TESTS` requirements in your PR when applicable.

## Commit & Pull Request Guidelines
Follow the existing imperative, <=72-character subject style (`Fix run_with_timeout on systems lacking killpg`). Commit in focused, reviewable chunks and call out regenerated test artifacts. Open PRs from feature branches into `devel`, include a concise summary plus test commands/results, and link related issues or release notes drafts. Add screenshots or terminal captures when demos or SDL output change; omit merge commits except for the automated `devel` → `main` promotion.

## Environment & Configuration Tips
Set `CLIKE_LIB_DIR` and `PASCAL_LIB_DIR` when working outside the repo root so imports resolve. Headless runs should export `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy`; document any other transient settings in PRs for reproducibility.
