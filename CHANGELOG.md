# Changelog

## v2.2 – 2025-09-18

Enhancements
- Pascal: Introduced address-of operator `@` for routines and first-class procedure/function pointer types.
- Pascal: Added indirect procedure-pointer calls in both expression and statement contexts.
- Rea (experimental): Added a new object‑oriented front end with classes, fields, methods, constructors, single inheritance via `extends`, virtual dispatch, and `my`/`myself`/`super` semantics. See `Docs/rea_*` and `Examples/rea/`.
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
- Installer: ensure `/usr/local/pscal/misc` exists before copying misc assets.

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
