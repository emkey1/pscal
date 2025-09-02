# Changelog

All notable changes to this project will be documented in this file.

## v2.1-beta – 2025-09-02

Highlights
- New compile-only flags for both front-ends: `--dump-bytecode` and `--dump-bytecode-only`.
- CLike: short-circuit `&&` and `||`, shift operator precedence (`<<`, `>>`), improved `~` behavior on integers.
- Tests and CI: expanded regression coverage and CI now compiles all Examples in dump-only mode.

Added
- Front-end flags:
  - CLike: `--dump-bytecode`, `--dump-bytecode-only`.
  - Pascal: `--dump-bytecode`, `--dump-bytecode-only`.
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

