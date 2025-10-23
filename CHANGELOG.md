# Changelog

## v3.0.0 – 2025-10-20

Enhancements
- Added the `exsh` shell front end with PSCAL builtin integration, Bash-compatible `-c` execution, and tuned caching/pipeline helpers for scripts.
- Threading helpers now include `ThreadSpawnBuiltin`, `ThreadGetResult`, and `ThreadGetStatus`, allowing exsh to launch allow-listed VM builtins on worker threads while `WaitForThread` reports their stored success flags.
- Replaced the VM builtin registry with hash-indexed lookups and cached procedure symbols, cutting dispatch overhead across all languages.
- Extended Pascal with `goto`/`label` support, compound assignment operators, and refreshed CRT demos such as the Blackjack scoreboard.
- Optimised tight VM paths by inlining guarded loops and introducing owned string helpers that trim transient allocations.

Stability and correctness
- Fixed Pascal dynamic-array `SetLength` and `Low/High` behaviour so nested data retains contents and bounds stay accurate.
- Hardened exsh errexit, redirection, and directory-stack handling to align parity tests and shellbench workloads with Bash.
- Patched Rea parser/CLI edge cases and expanded regression coverage with new scope suites and cached-bytecode guards.
- Ensured extended builtin detection stays stable alongside host thread helpers and optional module toggles.

Developer experience
- `Tests/run_all_tests` now auto-selects a writable `TMPDIR`, runs from any working directory, and leaves network suites opt-in.
- Rea's regression harness tolerates `set -u` shells and handles empty arg manifests without aborting.
- Expanded documentation for exsh debugging, compiler flags, and the Rea programmer workflow.
- Shared library runners spin up local HTTP helpers per language and summarise results consistently.
- Introduced `MStreamFromString` so front ends can materialise string payloads for socket APIs without scratch files.

Fixed
- Resolved Blackjack prompt placement and CRT scoreboard layout glitches.
- Restored SDL demo controls, fonts, and fast landscape rendering fallbacks across Pascal and Rea samples.
- Addressed shellbench performance regressions while keeping EXIT traps and status propagation correct.
- Eliminated builtin registry double frees and stale cache metadata introduced during hashing refactors.

## v2.3.0 – 2025-10-01

Enhancements
- Extended built-ins gained full sqlite and yyjson catalogs, a `--dump-ext-builtins` CLI switch, and keep builtin IDs stable across optional modules.
- Graphics helpers now register `glCullFace`, validate fast landscape rendering data, and let `GraphLoop` accept real delays with SDL and Rea fallbacks.
- Examples split into base/SDL tracks, fast landscape rendering ships enabled by default, and the Mandelbrot demo picks up a multi-threaded fast path.
- The project adopted the MIT license to match bundled third-party code.

Stability and correctness
- Compiler byte/integer coercions were tightened so byte parameters no longer crash integer-only builtins.
- Network and download helpers tolerate transport failures, cancellation, and Pascal warnings without aborting executions.
- Module caches handle growing import lists, extended builtin inputs are validated before rendering, and Rea `match` identifiers parse consistently.
- Rea comment heuristics were refined so integer division fixes no longer strip `//` comments mid-file.

Developer experience
- Library tests moved under `Tests/libs` with dedicated `run_{pascal,clike,rea}_tests.sh` entrypoints and consistent color baselines.
- Front ends and docs describe extended builtin discovery, and each binary can enumerate the active inventory.
- A hand-maintained Xcode project plus generator keeps IDE builds in sync with optional front ends and exposes configurable runners.

Fixed
- SDL landscape demos clamp water/terrain parameters, repair direction controls, and bake vertex data before rendering.
- Rea and SDL samples correct `OutTextXY`, falling piece rendering, and other regressions introduced during demo refactors.
- Plasma and weather demos render frames and report values correctly with progressive color updates and rounded temperatures.

## v2.21 – 2025-09-21

Enhancements
- Bytecode & VM: Added `TO_BOOL`, direct field/array load opcodes (including constant-index forms), and `CALL_BUILTIN_PROC`/`CALL_USER_PROC` so builtin and user procedures carry stable identifiers regardless of SDL support.
- Languages: Pascal/CLike emit the VM's XOR opcode with constant folding, Rea adds `nil`, and Pascal gained an `override builtin` directive plus ASCII-safe identifier lexing.
- CLI & Docs: Pascal, CLike, and Rea accept `--no-cache`; documentation covers the new builtin-call flow; a comprehensive `PerformanceBenchmark` example exercises the VM.

Stability and correctness
- Deferred global initializers until after vtables are emitted, ensuring zero-argument `new` calls and nested `CALL_USER_PROC` invocations wire constructors in source order.
- Pascal cached-bytecode detection no longer depends on `memmem`, musl builds tokenize identifiers reliably, and direct-load bytecode keeps cached locals metadata intact.
- Rea preserves method metadata across recompiles, fixes call-frame slot allocation, and regression tests verify the Hangman5 vtable appears before its global constructor.
- Mutex registries are shared by spawned threads and `INC_LOCAL`/`DEC_LOCAL` now adjust real and enum locals without type errors.

Developer experience
- Regression runners invoke every front end with `--no-cache`, normalize case-sensitive paths on macOS, and disassemble Hangman5 to guard vtable ordering.
- The build system probes for AddressSanitizer support before enabling `dascal` instrumentation.

Fixed
- Prevented console color resets from forcing black backgrounds and restored WordRepository's vtable emission ahead of global constructors.



## v2.2 – 2025-09-18

Enhancements
- Pascal: Introduced address-of operator `@` for routines and first-class procedure/function pointer types.
- Pascal: Added indirect procedure-pointer calls in both expression and statement contexts.
- Rea (experimental): Added a new object‑oriented front end with classes, fields, methods, constructors, single inheritance via `extends`, virtual dispatch, and `my`/`myself`/`super` semantics. See `Docs/rea_*` and `Examples/rea/base` or `Examples/rea/sdl`.
- Threads: Added `CreateThread(@Proc, arg)` and `WaitForThread(thread)` builtins; `CreateThread` remains backward-compatible with the 1‑arg form.
- VM: Implemented indirect call opcodes and support for passing an initial pointer argument to new threads.
- Tools: Added standalone bytecode decompiler `pscald` and AST JSON → Bytecode compiler `pscaljson2bc` (with optional Bash/Zsh completions).

Stability and correctness
- Pointer metadata propagation: pointer locals/globals now carry base‑type metadata end‑to‑end so `new(p)` and `p^` dereferences work reliably.
- Parser: supports named parameter syntax in procedure/function pointer types (e.g., `function(x: Integer): Integer`).
- Cache: improved staleness detection when source/cache mtimes match; cache invalidates when the binary is newer.

Developer experience
- Documentation updated for `@`, procedure/function pointers, indirect calls, thread APIs, AST JSON pipeline, and HTTP security.
- Build: macOS SDK auto‑detect; opt‑in `-DPSCAL_USE_BREW_CURL=ON` to prefer Homebrew curl; clean `-Wall` builds.

Fixed
- Installer: ensure `${PSCAL_INSTALL_ROOT}/misc` exists before copying misc assets.

All notable changes to this project will be documented in this file.

## v2.1-beta – 2025-09-02

Highlights
- New compile-only flags for both front-ends: `--dump-bytecode` and `--dump-bytecode-only`.
- Concurrency: lightweight threads (spawn/join) and mutexes (standard and recursive) available across front-ends.
- CLike: short-circuit `&&` and `||`, shift operator precedence (`<<`, `>>`), improved `~` behavior on integers.
- Tests and CI: expanded regression coverage and CI now compiles all Examples in dump-only mode.

Added
- Front-end flags:
  - CLike: `--dump-bytecode`, `--dump-bytecode-only`.
  - Pascal: `--dump-bytecode`, `--dump-bytecode-only`.
- VM concurrency APIs:
  - Thread spawn/join, mutex create/lock/unlock/destroy (including recursive mutex option).
- CLike language features:
  - Parser support for shift operators, placed between additive and relational precedence.
  - Short-circuit code generation for logical `&&` and `||`.
  - `~x` on integer operands behaves like bitwise NOT via `(-x - 1)`; non-integers fall back to logical NOT.
- Regression tests:
  - CLike: shift precedence (Shifts), short-circuit behavior (ShortCircuit), assignment expression value, division behavior.
  - Pascal: negative parser tests (MalformedUses, InvalidArrayBounds), text round-trip, mutex id reuse.
- CI enhancements:
  - Run CTest, both front-end test scripts, and compile all Examples in dump-only mode.

Changed
- CLike examples: `chudnovsky_native` constant updated to a real literal to satisfy `long double` typing.
- Documentation:
  - README: front-end options, operator semantics, environment variables, SDL headless guidance.
  - Docs/clike_tutorial: options, semantics (short-circuit, shifts, `~`), `#ifdef SDL_ENABLED`, imports, env vars.

Fixed
- Build: eliminated an unused local variable warning in CLike code generation for a clean release build.

Notes
- SDL: When built with `-DSDL=ON`, `SDL_ENABLED` is defined for CLike (`#ifdef SDL_ENABLED`). Test runners default to dummy drivers unless `RUN_SDL=1` is set.
- VM bytecode cache version: `PSCAL_VM_VERSION = 5`.
